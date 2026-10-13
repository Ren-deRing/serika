#include <serika/cpu.h>
#include <serika/errno.h>
#include <serika/fcntl.h>
#include <serika/mm.h>
#include <serika/net.h>
#include <serika/percpu.h>
#include <serika/poll.h>
#include <serika/sched.h>
#include <serika/signal.h>
#include <serika/slab.h>
#include <serika/socket.h>
#include <serika/string.h>
#include <serika/syscall.h>
#include <serika/task.h>
#include <serika/tcp.h>
#include <serika/uio.h>
#include <serika/vfs.h>

#include <asm/arch.h>

/* the calls of a stream socket on TCP. as the datagram ones, they take net_lock themselves, and nothing is copied to or from
 * user memory under it: a send is gathered in pieces of 4 KiB, a receive is copied out of the connection into a piece */

#define PIECE 4096

#define TCP_NODELAY     1
#define TCP_MAXSEG      2
#define TCP_CORK        3
#define TCP_KEEPIDLE    4
#define TCP_KEEPINTVL   5
#define TCP_KEEPCNT     6
#define TCP_DEFER_ACCEPT 9
#define TCP_QUICKACK    12
#define TCP_CONGESTION  13
#define TCP_USER_TIMEOUT 18
#define TCP_FASTOPEN    23

#define POLLRDHUP       0x2000

static list_node tcp_bound = LIST_HEAD_INIT(tcp_bound);   /* the sockets with a name, by their node */

static struct isock *isock_of(struct file *f) {
    return f->inode->priv;
}

static uint64_t deadline_of(uint64_t timeout) {
    return timeout ? rdtsc() + timeout : ~0ULL;
}

static bool overlap(uint32_t a, uint32_t b) {
    return a == 0 || b == 0 || a == b;
}

/* ---- names ---- */

#define REUSEADDR (1U << SO_REUSEADDR)

/* a port nobody has, for a connection to raddr:rport from laddr */
static uint16_t pick_port(uint32_t laddr, uint32_t raddr, uint16_t rport) {
    static uint16_t next = UDP_PORT_FIRST;

    for (unsigned tries = 0; tries < 65536 - UDP_PORT_FIRST; tries++) {
        uint16_t p = next;

        next = next == 65535 ? UDP_PORT_FIRST : next + 1;

        bool used = false;

        for (list_node *l = tcp_bound.next; l != &tcp_bound; l = l->next)
            if (container_of(l, struct isock, node)->lport == p) used = true;

        if (used || tcp_find_listener(laddr, p)) continue;
        if (raddr ? tcp_lookup(laddr, p, raddr, rport) != NULL : tcp_port_used(p)) continue;

        return p;
    }

    return 0;
}

static int do_bind(struct isock *sk, uint32_t addr, uint16_t port) {
    if (sk->bound) return -EINVAL;

    if (addr && !ip_is_local(addr) && (addr >> 24) != 127) return -EADDRNOTAVAIL;

    if (!port) {
        port = pick_port(addr, 0, 0);
        if (!port) return -EADDRINUSE;
    } else {
        for (list_node *l = tcp_bound.next; l != &tcp_bound; l = l->next) {
            struct isock *o = container_of(l, struct isock, node);

            if (o->lport != port || !overlap(o->laddr, addr)) continue;

            /* two that share the port: both said so, and the one that is there does not listen */
            if ((o->flags & REUSEADDR) && (sk->flags & REUSEADDR) && o->tcb->state != TCP_LISTEN) continue;

            return -EADDRINUSE;
        }

        /* connections of a port that was given up (waiting out their end) are in the way unless it is asked for */
        if (!(sk->flags & REUSEADDR)) {
            for (list_node *l = tcp_conns.next; l != &tcp_conns; l = l->next) {
                struct tcb *c = container_of(l, struct tcb, node);

                if (c->lport == port && overlap(c->laddr, addr)) return -EADDRINUSE;
            }
        }
    }

    sk->laddr = addr;
    sk->lport = port;
    sk->bound = true;
    list_add_tail(&sk->node, &tcp_bound);
    return 0;
}

long tcp_op_bind(struct file *f, long uaddr, long ulen) {
    struct isock *sk = isock_of(f);
    struct sockaddr_in sin;
    int err = sin_from_user(uaddr, ulen, &sin);

    if (err) return err;

    mutex_lock(&net_lock);
    err = do_bind(sk, ntohl(sin.addr), ntohs(sin.port));
    mutex_unlock(&net_lock);
    return err;
}

long tcp_op_getname(struct file *f, long uaddr, long ulenp, bool peer) {
    struct isock *sk = isock_of(f);
    struct sockaddr_in sin;

    mutex_lock(&net_lock);

    if (peer) {
        if (!sk->connected) {
            mutex_unlock(&net_lock);
            return -ENOTCONN;
        }

        sin_make(&sin, sk->raddr, sk->rport);
    } else {
        sin_make(&sin, sk->laddr ? sk->laddr : sk->tcb->laddr, sk->lport);
    }

    mutex_unlock(&net_lock);
    return sin_to_user(uaddr, ulenp, &sin);
}

/* ---- connect, listen, accept ---- */

long tcp_op_connect(struct file *f, long uaddr, long ulen) {
    struct isock *sk = isock_of(f);
    struct sockaddr_in sin;
    int err = sin_from_user(uaddr, ulen, &sin);

    if (err) return err;

    uint32_t dst = ntohl(sin.addr);
    uint16_t dport = ntohs(sin.port);
    bool nonblock = f->flags & O_NONBLOCK;

    if (!dport) return -EINVAL;
    if (!dst) dst = IP4(127, 0, 0, 1);

    mutex_lock(&net_lock);

    struct tcb *tp = sk->tcb;

    switch (tp->state) {
    case TCP_CLOSED:
        break;
    case TCP_SYN_SENT:
    case TCP_SYN_RECV:
        mutex_unlock(&net_lock);
        return -EALREADY;
    default:
        mutex_unlock(&net_lock);
        return -EISCONN;
    }

    uint32_t from = ip_src_for(dst);

    if (!from) {
        mutex_unlock(&net_lock);
        return -ENETUNREACH;
    }

    if (!sk->bound && (err = do_bind(sk, 0, 0))) {
        mutex_unlock(&net_lock);
        return err;
    }

    uint32_t laddr = sk->laddr ? sk->laddr : from;
    uint16_t lport = sk->lport;

    /* the port of a bind may be one that this connection is already on; a port of our own is chosen again then */
    if (tcp_lookup(laddr, lport, dst, dport)) {
        mutex_unlock(&net_lock);
        return -EADDRNOTAVAIL;
    }

    /* a connection that was before is not reused: a new one with the options of the old */
    struct tcb *nt = tcb_new();

    if (!nt) {
        mutex_unlock(&net_lock);
        return -ENOMEM;
    }

    nt->rcvbuf    = tp->rcvbuf;
    nt->sndbuf    = tp->sndbuf;
    nt->nodelay   = tp->nodelay;
    nt->keepalive = tp->keepalive;
    nt->ka_idle   = tp->ka_idle;
    nt->ka_intvl  = tp->ka_intvl;
    nt->ka_cnt    = tp->ka_cnt;

    tp->sk = NULL;
    tcb_free(tp);

    tp = sk->tcb = nt;
    tp->sk = sk;

    tp->laddr = laddr;
    tp->lport = lport;
    tp->raddr = dst;
    tp->rport = dport;

    tp->rcv_scale      = tcp_scale_for(tp->rcvbuf);
    tp->wscale_offered = true;

    tp->iss     = tcp_isn(laddr, lport, dst, dport);
    tp->snd_una = tp->iss;
    tp->snd_nxt = tp->snd_max = tp->snd_end = tp->iss + 1;
    tp->recover = tp->iss;
    tp->state   = TCP_SYN_SENT;

    list_add_tail(&tp->node, &tcp_conns);
    tp->listed = true;

    sk->connected = true;
    sk->raddr     = dst;
    sk->rport     = dport;

    tcp_send_syn(tp);
    tcp_start_rto(tp);

    if (nonblock) {
        mutex_unlock(&net_lock);
        return -EINPROGRESS;
    }

    err = net_wait_intr(tp->state != TCP_SYN_SENT && tp->state != TCP_SYN_RECV, deadline_of(sk->sndtimeo));

    if (err == -ERESTARTSYS) err = -EINTR;       /* it goes on by itself */
    else if (err == -ETIMEDOUT) err = -EINPROGRESS;

    if (!err && tp->state == TCP_CLOSED) {
        err = tp->err ? tp->err : -ECONNREFUSED;
        tp->err = 0;
        sk->connected = false;
    }

    mutex_unlock(&net_lock);
    return err;
}

long tcp_op_listen(struct file *f, long backlog) {
    struct isock *sk = isock_of(f);
    int err = 0;

    mutex_lock(&net_lock);

    struct tcb *tp = sk->tcb;

    if (tp->state != TCP_CLOSED && tp->state != TCP_LISTEN) {
        err = -EINVAL;
    } else if (!sk->bound && (err = do_bind(sk, 0, 0))) {
        /* err */
    } else {
        tp->backlog = backlog < 0 ? 0 : backlog > SOMAXCONN ? SOMAXCONN : backlog;

        if (tp->state == TCP_CLOSED) {
            tp->laddr = sk->laddr;
            tp->lport = sk->lport;
            tp->state = TCP_LISTEN;

            list_add_tail(&tp->node, &tcp_listen);
            tp->listed = true;
        }
    }

    mutex_unlock(&net_lock);
    return err;
}

long tcp_op_accept(struct file *f, long uaddr, long ulenp, long flags) {
    struct isock *sk = isock_of(f);
    bool nonblock = f->flags & O_NONBLOCK;

    mutex_lock(&net_lock);

    struct tcb *tp = sk->tcb;

    if (tp->state != TCP_LISTEN) {
        mutex_unlock(&net_lock);
        return -EINVAL;
    }

    while (list_empty(&tp->accept_q)) {
        if (nonblock) {
            mutex_unlock(&net_lock);
            return -EAGAIN;
        }

        int err = net_wait_intr(!list_empty(&tp->accept_q) || tp->state != TCP_LISTEN, deadline_of(sk->rcvtimeo));

        if (err == -ETIMEDOUT) err = -EAGAIN;

        if (err || tp->state != TCP_LISTEN) {
            mutex_unlock(&net_lock);
            return err ? err : -EINVAL;
        }
    }

    struct tcb *c = list_first(&tp->accept_q, struct tcb, accept_node);
    struct isock *nsk = isock_new(SOCK_STREAM, IPPROTO_TCP);

    if (!nsk) {
        mutex_unlock(&net_lock);
        return -ENOMEM;
    }

    list_del(&c->accept_node);
    tp->nr_accept--;
    c->in_accept = false;
    c->parent    = NULL;

    nsk->tcb       = c;
    c->sk          = nsk;
    nsk->laddr     = c->laddr;
    nsk->lport     = c->lport;
    nsk->raddr     = c->raddr;
    nsk->rport     = c->rport;
    nsk->connected = true;
    nsk->rcvbuf    = c->rcvbuf;
    nsk->sndbuf    = c->sndbuf;
    nsk->flags     = sk->flags;

    struct sockaddr_in peer;

    sin_make(&peer, c->raddr, c->rport);

    mutex_unlock(&net_lock);

    long fd = sock_fd_new(&nsk->sock, flags);

    if (fd < 0) return fd;

    if (uaddr && ulenp) {
        long err = sin_to_user(uaddr, ulenp, &peer);

        if (err) {
            sys_close(fd);
            return err;
        }
    }

    return fd;
}

long tcp_op_shutdown(struct file *f, long how) {
    struct isock *sk = isock_of(f);
    long err = 0;

    mutex_lock(&net_lock);

    struct tcb *tp = sk->tcb;

    if (tp->state == TCP_CLOSED || tp->state == TCP_LISTEN || tp->state == TCP_SYN_SENT || tp->state == TCP_SYN_RECV) {
        err = tp->state == TCP_CLOSED && sk->connected ? 0 : -ENOTCONN;
    } else {
        if (how == SHUT_RD || how == SHUT_RDWR) tp->rx_shut = true;

        if ((how == SHUT_WR || how == SHUT_RDWR) && !tp->fin_pending) {
            tp->fin_pending = true;
            tcp_output(tp);
        }
    }

    mutex_unlock(&net_lock);
    return err;
}

/* ---- data ---- */

/* why a send cannot go on, or 0. SYN states are waited out by the caller */
static long send_state(struct isock *sk, struct tcb *tp) {
    switch (tp->state) {
    case TCP_ESTABLISHED:
    case TCP_CLOSE_WAIT:
        return tp->fin_pending ? -EPIPE : 0;

    case TCP_SYN_SENT:
    case TCP_SYN_RECV:
        return 0;

    case TCP_CLOSED:
        if (tp->err) {
            long e = tp->err;

            tp->err = 0;
            return e;
        }

        return sk->connected ? -EPIPE : -ENOTCONN;

    case TCP_LISTEN:
        return -ENOTCONN;

    default:
        return -EPIPE;
    }
}

/* len bytes of kernel memory to the connection: the bytes that went in, or an error if none did. waits for room unless nonblock */
static long send_buf(struct isock *sk, const uint8_t *data, size_t len, bool nonblock) {
    size_t done = 0;
    long err = 0;

    mutex_lock(&net_lock);

    struct tcb *tp = sk->tcb;

    while (done < len) {
        err = send_state(sk, tp);

        if (err) break;

        if (tp->state == TCP_SYN_SENT || tp->state == TCP_SYN_RECV) {
            if (nonblock) {
                err = -EAGAIN;
                break;
            }

            err = net_wait_intr(tp->state != TCP_SYN_SENT && tp->state != TCP_SYN_RECV, deadline_of(sk->sndtimeo));
            if (err) break;
            continue;
        }

        if (tp->snd_bytes >= (uint32_t)tp->sndbuf) {
            if (nonblock) {
                err = -EAGAIN;
                break;
            }

            err = net_wait_intr(tp->snd_bytes < (uint32_t)tp->sndbuf || tp->state == TCP_CLOSED || tp->fin_pending ||
                                (tp->state != TCP_ESTABLISHED && tp->state != TCP_CLOSE_WAIT), deadline_of(sk->sndtimeo));

            if (err == -ETIMEDOUT) err = -EAGAIN;
            if (err) break;
            continue;
        }

        uint32_t q = tcp_queue(tp, data + done, len - done);

        if (!q) {
            err = -ENOMEM;
            break;
        }

        done += q;
        tcp_output(tp);
    }

    mutex_unlock(&net_lock);

    return done ? (long)done : err;
}

/* up to want bytes of data into kb (skipping `skip` of it for a peek). n, 0 at the end of the stream, or -errno. -EAGAIN: nothing
 * now (and block is false) */
static long recv_buf(struct isock *sk, uint8_t *kb, size_t want, size_t skip, bool peek, bool block) {
    mutex_lock(&net_lock);

    struct tcb *tp = sk->tcb;
    long r;

    for (;;) {
        if (tp->rx_bytes > skip) {
            size_t n = 0, off = 0;

            list_node *l = tp->rxq.next;

            while (l != &tp->rxq && n < want) {
                struct nbuf *b = container_of(l, struct nbuf, node);

                l = l->next;

                size_t have = b->len, from = 0;

                if (off < skip) {
                    size_t s = skip - off < have ? skip - off : have;

                    off += s;
                    from = s;
                    have -= s;
                }

                if (!have) continue;

                size_t take = have < want - n ? have : want - n;

                memcpy(kb + n, b->data + from, take);
                n += take;

                if (!peek) {
                    nbuf_pull(b, take);

                    if (!b->len) {
                        list_del(&b->node);
                        nbuf_free(b);
                    }
                }
            }

            if (!peek) {
                tp->rx_bytes -= n;
                tcp_rcv_consumed(tp);
            }

            r = n;
            break;
        }

        if (tp->state == TCP_LISTEN) {
            r = -EINVAL;
            break;
        }

        if (tp->state == TCP_CLOSED && tp->err) {
            r = tp->err;
            tp->err = 0;
            break;
        }

        if (tp->state == TCP_CLOSED && !sk->connected) {
            r = -ENOTCONN;
            break;
        }

        if (tp->rx_shut || tp->fin_rcvd || tp->state == TCP_CLOSED) {
            r = 0;
            break;
        }

        if (!block) {
            r = -EAGAIN;
            break;
        }

        r = net_wait_intr(tp->rx_bytes > skip || tp->rx_shut || tp->fin_rcvd || tp->state == TCP_CLOSED,
                          deadline_of(sk->rcvtimeo));

        if (r == -ETIMEDOUT) {
            r = -EAGAIN;
            break;
        }

        if (r) break;
    }

    mutex_unlock(&net_lock);
    return r;
}

long tcp_op_sendmsg(struct file *f, struct msghdr *mh, const struct iovec *iov, unsigned flags) {
    struct isock *sk = isock_of(f);
    bool nonblock = (f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT);
    size_t total = 0;

    for (size_t i = 0; i < mh->msg_iovlen; i++) {
        if (iov[i].len > 0x7fffffff || total + iov[i].len > 0x7fffffff) return -EINVAL;
        total += iov[i].len;
    }

    uint8_t *kb = kmalloc(PIECE);
    if (!kb) return -ENOMEM;

    size_t sent = 0, vec = 0, voff = 0;
    long err = 0;

    while (sent < total) {
        size_t chunk = total - sent < PIECE ? total - sent : PIECE;

        for (size_t got = 0; got < chunk;) {
            while (voff == iov[vec].len) {
                vec++;
                voff = 0;
            }

            size_t take = iov[vec].len - voff;

            if (take > chunk - got) take = chunk - got;

            if ((err = copy_from_user(kb + got, iov[vec].base + voff, take))) goto out;

            got += take;
            voff += take;
        }

        long r = send_buf(sk, kb, chunk, nonblock);

        if (r < 0) {
            err = r;
            break;
        }

        sent += r;

        if ((size_t)r < chunk) break;            /* the room ran out (nonblocking) */
    }

out:
    kfree(kb);

    if (err == -EPIPE && !(flags & MSG_NOSIGNAL)) send_sigpipe();

    return sent ? (long)sent : err;
}

long tcp_op_recvmsg(struct file *f, struct msghdr *mh, const struct iovec *iov, unsigned flags) {
    struct isock *sk = isock_of(f);
    bool nonblock = (f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT);
    bool peek = flags & MSG_PEEK, waitall = (flags & MSG_WAITALL) && !peek;
    size_t cap = 0;

    for (size_t i = 0; i < mh->msg_iovlen; i++) {
        if (iov[i].len > 0x7fffffff || cap + iov[i].len > 0x7fffffff) return -EINVAL;
        cap += iov[i].len;
    }

    uint8_t *kb = kmalloc(PIECE);
    if (!kb) return -ENOMEM;

    size_t got = 0, vec = 0, voff = 0;
    long err = 0;

    while (got < cap) {
        size_t chunk = cap - got < PIECE ? cap - got : PIECE;

        /* the first piece waits (unless nonblock), the others only take what is there, unless all of it is asked for */
        bool block = !nonblock && (got == 0 || waitall);
        long r = recv_buf(sk, kb, chunk, peek ? got : 0, peek, block);

        if (r < 0) {
            if (!got && !(r == -EAGAIN && got)) err = r;
            break;
        }

        if (r == 0) break;

        for (size_t off = 0; off < (size_t)r;) {
            while (voff == iov[vec].len) {
                vec++;
                voff = 0;
            }

            size_t take = iov[vec].len - voff;

            if (take > (size_t)r - off) take = r - off;

            if ((err = copy_to_user(iov[vec].base + voff, kb + off, take))) goto out;

            off += take;
            voff += take;
        }

        got += r;

        if (peek) break;                         /* a peek is one look */
    }

out:
    kfree(kb);

    mh->msg_namelen    = 0;
    mh->msg_controllen = 0;
    mh->msg_flags      = 0;

    return err && !got ? err : (long)got;
}

long tcp_op_read(struct file *f, void *buf, size_t n) {
    return recv_buf(isock_of(f), buf, n, 0, false, !(f->flags & O_NONBLOCK));
}

long tcp_op_write(struct file *f, const void *buf, size_t n) {
    long r = send_buf(isock_of(f), buf, n, f->flags & O_NONBLOCK);

    if (r == -EPIPE) send_sigpipe();
    return r;
}

uint32_t tcp_op_poll(struct inode *inode, struct poll_table *pt) {
    struct isock *sk = inode->priv;
    uint32_t mask = 0;

    poll_wait(pt, &net_wq);

    mutex_lock(&net_lock);

    struct tcb *tp = sk->tcb;
    bool room = (int)tp->snd_bytes + tp->sndbuf / 4 <= tp->sndbuf;

    switch (tp->state) {
    case TCP_LISTEN:
        if (!list_empty(&tp->accept_q)) mask |= POLLIN;
        break;

    case TCP_CLOSED:
        mask |= POLLHUP | POLLOUT;
        if (tp->rx_bytes) mask |= POLLIN;
        if (tp->err) mask |= POLLERR;
        break;

    case TCP_SYN_SENT:
    case TCP_SYN_RECV:
        break;

    default:
        if (tp->rx_bytes || tp->fin_rcvd || tp->rx_shut) mask |= POLLIN;
        if (tp->fin_rcvd) mask |= POLLRDHUP;
        if ((tp->state == TCP_ESTABLISHED || tp->state == TCP_CLOSE_WAIT) && !tp->fin_pending && room) mask |= POLLOUT;
        if (tp->state == TCP_CLOSING || tp->state == TCP_LAST_ACK || tp->state == TCP_TIME_WAIT) mask |= POLLHUP;
        break;
    }

    mutex_unlock(&net_lock);
    return mask;
}

/* ---- options ---- */

long tcp_op_setsockopt(struct file *f, long level, long name, long uval, long len) {
    struct isock *sk = isock_of(f);
    int32_t v = 0;

    if (level != SOL_TCP) return -ENOPROTOOPT;

    switch (name) {
    case TCP_NODELAY: case TCP_MAXSEG: case TCP_CORK: case TCP_KEEPIDLE: case TCP_KEEPINTVL: case TCP_KEEPCNT:
    case TCP_DEFER_ACCEPT: case TCP_QUICKACK: case TCP_USER_TIMEOUT: case TCP_FASTOPEN:
        break;

    case TCP_CONGESTION:
        return 0;                                 /* it is Reno whatever is asked */

    default:
        return -ENOPROTOOPT;
    }

    if (len < (long)sizeof(v)) return -EINVAL;
    if (copy_from_user(&v, uval, sizeof(v))) return -EFAULT;

    mutex_lock(&net_lock);

    struct tcb *tp = sk->tcb;
    long err = 0;

    switch (name) {
    case TCP_NODELAY:
        tp->nodelay = v != 0;

        if (tp->nodelay) tcp_output(tp);          /* what Nagle held back */
        break;

    case TCP_KEEPIDLE:
    case TCP_KEEPINTVL:
    case TCP_KEEPCNT:
        if (v < 1 || v > 32767) {
            err = -EINVAL;
        } else if (name == TCP_KEEPIDLE) {
            tp->ka_idle = v;
        } else if (name == TCP_KEEPINTVL) {
            tp->ka_intvl = v;
        } else {
            tp->ka_cnt = v;
        }
        break;

    default:
        break;                                    /* taken, and nothing changes */
    }

    mutex_unlock(&net_lock);
    return err;
}

long tcp_op_getsockopt(struct file *f, long level, long name, long uval, long ulenp) {
    struct isock *sk = isock_of(f);
    int32_t ulen;
    long err;

    if (level != SOL_TCP) return -ENOPROTOOPT;
    if ((err = copy_from_user(&ulen, ulenp, sizeof(ulen)))) return err;
    if (ulen < 0) return -EINVAL;

    union { int32_t i; char s[16]; } out = { 0 };
    size_t size = sizeof(int32_t);

    mutex_lock(&net_lock);

    struct tcb *tp = sk->tcb;

    switch (name) {
    case TCP_NODELAY:   out.i = tp->nodelay; break;
    case TCP_MAXSEG:    out.i = tp->mss; break;
    case TCP_KEEPIDLE:  out.i = tp->ka_idle; break;
    case TCP_KEEPINTVL: out.i = tp->ka_intvl; break;
    case TCP_KEEPCNT:   out.i = tp->ka_cnt; break;
    case TCP_CORK:
    case TCP_QUICKACK:  out.i = 0; break;

    case TCP_CONGESTION:
        memcpy(out.s, "reno", 5);
        size = 5;
        break;

    default:
        err = -ENOPROTOOPT;
    }

    mutex_unlock(&net_lock);

    if (err) return err;

    int32_t n = ulen < (int32_t)size ? ulen : (int32_t)size, outlen = size;

    if ((n && (err = copy_to_user(uval, &out, n))) || (err = copy_to_user(ulenp, &outlen, sizeof(outlen)))) return err;

    return 0;
}

/* ---- the file ---- */

void tcp_sock_release(struct isock *sk) {
    mutex_lock(&net_lock);

    if (sk->bound) {
        list_del(&sk->node);
        sk->bound = false;
    }

    if (sk->tcb) {
        tcp_close(sk->tcb);
        sk->tcb = NULL;
    }

    mutex_unlock(&net_lock);
}

void tcp_op_release(struct inode *inode) {
    tcp_sock_release(inode->priv);
}
