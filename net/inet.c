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
#include <serika/task.h>
#include <serika/tcp.h>
#include <serika/uio.h>
#include <serika/vfs.h>

#include <asm/arch.h>

/* the AF_INET family: the calls of socket(2) on a UDP socket, and the ioctls that tell and set what an interface is. a
 * socket is under net_lock, and no lock is held across a copy to or from user memory */

#define AF_UNSPEC   0
#define IPPROTO_IP  0
#define IP_TOS      1
#define IP_TTL      2
#define IP_PKTINFO  8
#define IP_RECVERR  11
#define IP_MTU_DISCOVER 10

#define OPT_MIN_BUF 4608
#define OPT_MAX_BUF (8 << 20)

#define FIONREAD 0x541b
#define SIOCOUTQ 0x5411

#define SIOCGIFNAME    0x8910
#define SIOCGIFCONF    0x8912
#define SIOCGIFFLAGS   0x8913
#define SIOCSIFFLAGS   0x8914
#define SIOCGIFADDR    0x8915
#define SIOCSIFADDR    0x8916
#define SIOCGIFBRDADDR 0x8919
#define SIOCGIFNETMASK 0x891b
#define SIOCSIFNETMASK 0x891c
#define SIOCGIFMTU     0x8921
#define SIOCGIFHWADDR  0x8927
#define SIOCGIFINDEX   0x8933

#define IFF_UP        0x1
#define IFF_BROADCAST 0x2
#define IFF_LOOPBACK  0x8
#define IFF_RUNNING   0x40
#define IFF_MULTICAST 0x1000

#define ARPHRD_ETHER    1
#define ARPHRD_LOOPBACK 772

struct ifreq {
    char name[16];
    union {
        struct sockaddr_in sin;
        uint16_t           flags;
        int32_t            ivalue;
        struct { uint16_t family; uint8_t data[14]; } hw;
        uint8_t            pad[24];
    } u;
};

struct ifconf {
    int32_t  len;
    int32_t  pad;
    uint64_t buf;
};

struct timeval_user {
    int64_t sec;
    int64_t usec;
};

extern const struct proto_ops inet_proto_ops;

struct isock *isock_new(int type, int protocol) {
    struct isock *sk = kmalloc(sizeof(*sk));
    if (!sk) return NULL;

    memset(sk, 0, sizeof(*sk));

    sk->sock.ops = &inet_proto_ops;
    sk->type     = type;
    sk->protocol = protocol;
    sk->rcvbuf   = sk->sndbuf = UDP_RCVBUF;

    list_init(&sk->rx);
    return sk;
}

struct sock *inet_create(int type, int protocol, int *err) {
    if (type != SOCK_DGRAM && type != SOCK_STREAM) {
        *err = -ESOCKTNOSUPPORT;
        return NULL;
    }

    int proto = type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP;

    if (protocol != 0 && protocol != proto) {
        *err = -EPROTONOSUPPORT;
        return NULL;
    }

    struct isock *sk = isock_new(type, proto);

    if (!sk) {
        *err = -ENOMEM;
        return NULL;
    }

    if (type == SOCK_STREAM) {
        sk->tcb = tcb_new();

        if (!sk->tcb) {
            kfree(sk);
            *err = -ENOMEM;
            return NULL;
        }

        sk->tcb->sk = sk;
    }

    return &sk->sock;
}

static struct isock *isock_of(struct file *f) {
    return f->inode->priv;
}

/* ---- addresses ---- */

int sin_from_user(long uaddr, long ulen, struct sockaddr_in *sin) {
    if (ulen < (long)sizeof(*sin)) return -EINVAL;

    int err = copy_from_user(sin, uaddr, sizeof(*sin));
    if (err) return err;

    return sin->family == AF_INET ? 0 : -EAFNOSUPPORT;
}

void sin_make(struct sockaddr_in *sin, uint32_t addr, uint16_t port) {
    memset(sin, 0, sizeof(*sin));
    sin->family = AF_INET;
    sin->port   = htons(port);
    sin->addr   = htonl(addr);
}

/* as many bytes of it as fit in room, and the length it has. the length is in user memory too: read first, written after */
long sin_to_user(long uaddr, long ulenp, const struct sockaddr_in *sin) {
    int32_t room, total = sizeof(*sin);
    long err = copy_from_user(&room, ulenp, sizeof(room));

    if (err) return err;
    if (room < 0) return -EINVAL;

    if (room && (err = copy_to_user(uaddr, sin, room < total ? room : total))) return err;

    return copy_to_user(ulenp, &total, sizeof(total));
}

static int inet_op_bind(struct file *f, long uaddr, long ulen) {
    if (isock_of(f)->type == SOCK_STREAM) return tcp_op_bind(f, uaddr, ulen);

    struct isock *sk = isock_of(f);
    struct sockaddr_in sin;
    int err = sin_from_user(uaddr, ulen, &sin);

    if (err) return err;

    mutex_lock(&net_lock);
    err = udp_bind(sk, ntohl(sin.addr), ntohs(sin.port));
    mutex_unlock(&net_lock);
    return err;
}

static int inet_op_connect(struct file *f, long uaddr, long ulen) {
    if (isock_of(f)->type == SOCK_STREAM) return tcp_op_connect(f, uaddr, ulen);

    struct isock *sk = isock_of(f);
    struct sockaddr_in sin;
    uint16_t family;
    int err;

    /* the family AF_UNSPEC points it at nothing again */
    if (ulen >= 2 && copy_from_user(&family, uaddr, sizeof(family)) == 0 && family == AF_UNSPEC) {
        mutex_lock(&net_lock);
        sk->connected = false;
        mutex_unlock(&net_lock);
        return 0;
    }

    if ((err = sin_from_user(uaddr, ulen, &sin))) return err;
    if (sin.port == 0) return -EINVAL;

    mutex_lock(&net_lock);

    if (!ip_src_for(ntohl(sin.addr))) {
        err = -ENETUNREACH;
    } else if (!sk->bound && (err = udp_bind(sk, 0, 0))) {
        /* err */
    } else {
        sk->connected = true;
        sk->raddr     = ntohl(sin.addr);
        sk->rport     = ntohs(sin.port);
        err           = 0;
    }

    mutex_unlock(&net_lock);
    return err;
}

static int inet_op_listen(struct file *f, long backlog) {
    if (isock_of(f)->type == SOCK_STREAM) return tcp_op_listen(f, backlog);

    (void)f;
    (void)backlog;

    return -EOPNOTSUPP;
}

static long inet_op_accept(struct file *f, long uaddr, long ulenp, long flags) {
    if (isock_of(f)->type == SOCK_STREAM) return tcp_op_accept(f, uaddr, ulenp, flags);

    (void)f;
    (void)uaddr;
    (void)ulenp;
    (void)flags;

    return -EOPNOTSUPP;
}

static long inet_op_getname(struct file *f, long uaddr, long ulenp, bool peer) {
    if (isock_of(f)->type == SOCK_STREAM) return tcp_op_getname(f, uaddr, ulenp, peer);

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
        sin_make(&sin, sk->laddr, sk->lport);
    }

    mutex_unlock(&net_lock);
    return sin_to_user(uaddr, ulenp, &sin);
}

static int inet_op_shutdown(struct file *f, long how) {
    if (isock_of(f)->type == SOCK_STREAM) return tcp_op_shutdown(f, how);

    struct isock *sk = isock_of(f);

    mutex_lock(&net_lock);

    int err = 0;

    if (!sk->connected) {
        err = -ENOTCONN;
    } else {
        if (how == SHUT_RD || how == SHUT_RDWR) sk->rx_shut = true;
        if (how == SHUT_WR || how == SHUT_RDWR) sk->tx_shut = true;

        wake_up(&net_wq);
    }

    mutex_unlock(&net_lock);
    return err;
}

/* ---- data ---- */

/* the datagram of kernel memory to dst, or to the peer if dst is NULL */
static long send_k(struct isock *sk, const struct sockaddr_in *dst, const void *data, size_t len, unsigned flags) {
    uint32_t addr;
    uint16_t port;

    (void)flags;

    mutex_lock(&net_lock);

    long err = 0;

    if (sk->tx_shut) {
        err = -EPIPE;
    } else if (dst) {
        addr = ntohl(dst->addr);
        port = ntohs(dst->port);
        err  = 0;
    } else if (sk->connected) {
        addr = sk->raddr;
        port = sk->rport;
    } else {
        err = -EDESTADDRREQ;
    }

    if (!err) {
        /* a broadcast has to be asked for */
        bool bcast = addr == 0xffffffffU;

        for (struct netdev *d = netdev_first(); d; d = d->next)
            if (d->ip && addr == (d->ip | ~d->mask) && !(d->flags & NETDEV_LOOPBACK)) bcast = true;

        if (bcast && !(sk->flags & (1U << SO_BROADCAST))) err = -EACCES;
        else err = udp_send(sk, addr, port, data, len);
    }

    mutex_unlock(&net_lock);
    return err ? err : (long)len;
}

static long inet_op_sendmsg(struct file *f, struct msghdr *mh, const struct iovec *iov, unsigned flags) {
    if (isock_of(f)->type == SOCK_STREAM) return tcp_op_sendmsg(f, mh, iov, flags);

    struct isock *sk = isock_of(f);
    struct sockaddr_in sin;
    size_t total = 0;
    long err;

    for (size_t i = 0; i < mh->msg_iovlen; i++) {
        if (iov[i].len > 0x7fffffff || total + iov[i].len > 0x7fffffff) return -EINVAL;
        total += iov[i].len;
    }

    if (total > UDP_MAX) return -EMSGSIZE;

    if (mh->msg_name && (err = sin_from_user(mh->msg_name, mh->msg_namelen, &sin))) return err;

    uint8_t *kb = kmalloc(UDP_MAX);
    if (!kb) return -ENOMEM;

    size_t off = 0;

    for (size_t i = 0; i < mh->msg_iovlen; i++) {
        if ((err = copy_from_user(kb + off, iov[i].base, iov[i].len))) {
            kfree(kb);
            return err;
        }

        off += iov[i].len;
    }

    err = send_k(sk, mh->msg_name ? &sin : NULL, kb, total, flags);
    kfree(kb);

    if (err == -EPIPE && !(flags & MSG_NOSIGNAL)) send_sigpipe();
    return err;
}

static uint64_t rcv_deadline(struct isock *sk) {
    return sk->rcvtimeo ? rdtsc() + sk->rcvtimeo : ~0ULL;
}

static long inet_op_recvmsg(struct file *f, struct msghdr *mh, const struct iovec *iov, unsigned flags) {
    if (isock_of(f)->type == SOCK_STREAM) return tcp_op_recvmsg(f, mh, iov, flags);

    struct isock *sk = isock_of(f);
    bool nonblock = (f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT);
    size_t cap = 0, full = 0;
    uint32_t saddr = 0;
    uint16_t sport = 0;
    long err;

    for (size_t i = 0; i < mh->msg_iovlen; i++) {
        if (iov[i].len > 0x7fffffff || cap + iov[i].len > 0x7fffffff) return -EINVAL;
        cap += iov[i].len;
    }

    if (cap > UDP_MAX) cap = UDP_MAX;

    uint8_t *kb = kmalloc(UDP_MAX);
    if (!kb) return -ENOMEM;

    mutex_lock(&net_lock);
    long got = udp_recv(sk, kb, cap, &full, &saddr, &sport, flags & MSG_PEEK, nonblock, rcv_deadline(sk));
    mutex_unlock(&net_lock);

    if (got < 0) {
        kfree(kb);
        return got;
    }

    size_t off = 0;

    for (size_t i = 0; i < mh->msg_iovlen && off < (size_t)got; i++) {
        size_t n = iov[i].len < (size_t)got - off ? iov[i].len : (size_t)got - off;

        if ((err = copy_to_user(iov[i].base, kb + off, n))) {
            kfree(kb);
            return err;
        }

        off += n;
    }

    kfree(kb);

    mh->msg_flags = full > (size_t)got ? MSG_TRUNC : 0;

    if (mh->msg_name) {
        struct sockaddr_in sin;

        sin_make(&sin, saddr, sport);

        if (full || got) {
            size_t n = mh->msg_namelen < sizeof(sin) ? mh->msg_namelen : sizeof(sin);

            if (n && (err = copy_to_user(mh->msg_name, &sin, n))) return err;
        }

        mh->msg_namelen = sizeof(sin);
    }

    mh->msg_controllen = 0;

    /* a datagram says how long it was, if asked */
    return (flags & MSG_TRUNC) ? (long)full : got;
}

static long inet_op_read(struct file *f, void *buf, size_t n) {
    if (isock_of(f)->type == SOCK_STREAM) return tcp_op_read(f, buf, n);

    struct isock *sk = isock_of(f);
    size_t full;

    mutex_lock(&net_lock);
    long r = udp_recv(sk, buf, n, &full, NULL, NULL, false, f->flags & O_NONBLOCK, rcv_deadline(sk));
    mutex_unlock(&net_lock);
    return r;
}

static long inet_op_write(struct file *f, const void *buf, size_t n) {
    if (isock_of(f)->type == SOCK_STREAM) return tcp_op_write(f, buf, n);

    long r = send_k(isock_of(f), NULL, buf, n, 0);

    if (r == -EPIPE) send_sigpipe();
    return r;
}

static uint32_t inet_op_poll(struct inode *inode, struct poll_table *pt) {
    if (((struct isock *)inode->priv)->type == SOCK_STREAM) return tcp_op_poll(inode, pt);

    poll_wait(pt, &net_wq);

    mutex_lock(&net_lock);
    uint32_t mask = udp_poll(inode->priv);
    mutex_unlock(&net_lock);
    return mask;
}

/* ---- options ---- */

static bool opt_is_flag(long name) {
    switch (name) {
        case SO_DEBUG:
        case SO_REUSEADDR:
        case SO_DONTROUTE:
        case SO_BROADCAST:
        case SO_KEEPALIVE:
        case SO_OOBINLINE:
        case SO_NO_CHECK:
        case SO_REUSEPORT:
        case SO_PASSSEC:
            return true;
    }

    return false;
}

static long inet_op_setsockopt(struct file *f, long level, long name, long uval, long len) {
    struct isock *sk = isock_of(f);
    int32_t v = 0;
    long err = 0;

    if (level == SOL_TCP) return sk->type == SOCK_STREAM ? tcp_op_setsockopt(f, level, name, uval, len) : -ENOPROTOOPT;

    if (level == IPPROTO_IP) {
        /* what the resolver of glibc asks for, and nothing here changes with it */
        switch (name) {
            case IP_TOS: case IP_TTL: case IP_PKTINFO: case IP_RECVERR: case IP_MTU_DISCOVER: return 0;
        }

        return -ENOPROTOOPT;
    }

    if (level != SOL_SOCKET) return -ENOPROTOOPT;

    if (name == SO_RCVTIMEO || name == SO_SNDTIMEO) {
        struct timeval_user tv;

        if (len < (long)sizeof(tv)) return -EINVAL;
        if (copy_from_user(&tv, uval, sizeof(tv))) return -EFAULT;
        if (tv.sec < 0 || tv.usec < 0 || tv.usec >= 1000000) return -EDOM;

        uint64_t freq = this_cpu_read(tsc_freq_hz);
        uint64_t ticks = tv.sec * freq + tv.usec * freq / 1000000;

        mutex_lock(&net_lock);
        if (name == SO_RCVTIMEO) sk->rcvtimeo = ticks;
        else                     sk->sndtimeo = ticks;
        mutex_unlock(&net_lock);
        return 0;
    }

    if (name == SO_LINGER) return len < 8 ? -EINVAL : 0;

    if (len < (long)sizeof(v)) return -EINVAL;
    if (copy_from_user(&v, uval, sizeof(v))) return -EFAULT;

    mutex_lock(&net_lock);

    if (name == SO_SNDBUF || name == SO_RCVBUF || name == SO_SNDBUFFORCE || name == SO_RCVBUFFORCE) {
        int64_t size = 2 * (int64_t)v; /* as linux: it is told back doubled */

        if (size < OPT_MIN_BUF) size = OPT_MIN_BUF;
        if (size > OPT_MAX_BUF * 2) size = OPT_MAX_BUF * 2;

        if (name == SO_SNDBUF || name == SO_SNDBUFFORCE) sk->sndbuf = size;
        else                                             sk->rcvbuf = size;
    } else if (opt_is_flag(name)) {
        if (v) sk->flags |= 1U << name;
        else   sk->flags &= ~(1U << name);
    } else if (name != SO_PRIORITY && name != SO_RCVLOWAT && name != SO_SNDLOWAT && name != SO_PASSCRED) {
        err = -ENOPROTOOPT;
    }

    /* the connection has its own copy of what it needs */
    if (!err && sk->type == SOCK_STREAM && sk->tcb) {
        struct tcb *tp = sk->tcb;
        bool ka = sk->flags & (1U << SO_KEEPALIVE);

        tp->rcvbuf = sk->rcvbuf;
        tp->sndbuf = sk->sndbuf;

        if (ka != tp->keepalive) {
            tp->keepalive = ka;

            if (ka) tcp_keepalive_start(tp);
            else tp->ka_at = 0;
        }
    }

    mutex_unlock(&net_lock);
    return err;
}

static long inet_op_getsockopt(struct file *f, long level, long name, long uval, long ulenp) {
    struct isock *sk = isock_of(f);
    int32_t ulen;
    long err;

    if (level == SOL_TCP) return sk->type == SOCK_STREAM ? tcp_op_getsockopt(f, level, name, uval, ulenp) : -ENOPROTOOPT;

    if ((err = copy_from_user(&ulen, ulenp, sizeof(ulen)))) return err;
    if (ulen < 0) return -EINVAL;

    union {
        int32_t             i;
        struct timeval_user tv;
        int32_t             linger[2];
    } out = { 0 };

    size_t size = sizeof(int32_t);

    if (level == IPPROTO_IP) {
        switch (name) {
            case IP_TTL: out.i = 64; break;
            case IP_TOS: out.i = 0; break;
            default:     return -ENOPROTOOPT;
        }
    } else if (level != SOL_SOCKET) {
        return -ENOPROTOOPT;
    } else {
        mutex_lock(&net_lock);

        switch (name) {
            case SO_TYPE:        out.i = sk->type; break;
            case SO_ERROR:
                /* the error nobody has been told of, once */
                if (sk->type == SOCK_STREAM && sk->tcb) {
                    out.i = -sk->tcb->err;
                    sk->tcb->err = 0;
                } else {
                    out.i = -sk->err;
                    sk->err = 0;
                }
                break;

            case SO_ACCEPTCONN:  out.i = sk->type == SOCK_STREAM && sk->tcb && sk->tcb->state == TCP_LISTEN; break;
            case SO_SNDBUF:
            case SO_SNDBUFFORCE: out.i = sk->sndbuf; break;
            case SO_RCVBUF:
            case SO_RCVBUFFORCE: out.i = sk->rcvbuf; break;
            case SO_DOMAIN:      out.i = AF_INET; break;
            case SO_PROTOCOL:    out.i = sk->protocol; break;
            case SO_PRIORITY:
            case SO_PASSCRED:    out.i = 0; break;
            case SO_RCVLOWAT:
            case SO_SNDLOWAT:    out.i = 1; break;
            case SO_LINGER:      size = sizeof(out.linger); break;

            case SO_RCVTIMEO:
            case SO_SNDTIMEO: {
                uint64_t freq = this_cpu_read(tsc_freq_hz), t = name == SO_RCVTIMEO ? sk->rcvtimeo : sk->sndtimeo;

                size = sizeof(out.tv);
                out.tv.sec = t / freq;
                out.tv.usec = t % freq * 1000000 / freq;
                break;
            }

            default:
                if (opt_is_flag(name)) out.i = (sk->flags >> name) & 1;
                else                   err = -ENOPROTOOPT;
        }

        mutex_unlock(&net_lock);

        if (err) return err;
    }

    int32_t n = ulen < (int32_t)size ? ulen : (int32_t)size, outlen = size;

    if ((n && (err = copy_to_user(uval, &out, n))) || (err = copy_to_user(ulenp, &outlen, sizeof(outlen)))) return err;

    return 0;
}

/* ---- ioctls: the interfaces ---- */

static struct netdev *dev_by_name(const char *name) {
    for (struct netdev *d = netdev_first(); d; d = d->next)
        if (strcmp(d->name, name) == 0) return d;

    return NULL;
}

static int dev_index(const struct netdev *dev) {
    int i = 1;

    for (struct netdev *d = netdev_first(); d; d = d->next, i++)
        if (d == dev) return i;

    return 0;
}

static uint32_t default_mask(uint32_t ip) {
    if ((ip >> 31) == 0) return IP4(255, 0, 0, 0);
    if ((ip >> 30) == 2) return IP4(255, 255, 0, 0);
    return IP4(255, 255, 255, 0);
}

static long ifconf(unsigned long arg) {
    struct ifconf ic;
    long err = copy_from_user(&ic, arg, sizeof(ic));

    if (err) return err;

    int count = 0;

    mutex_lock(&net_lock);

    for (struct netdev *d = netdev_first(); d; d = d->next) {
        if (!d->ip) continue;

        if (ic.buf && (count + 1) * (int)sizeof(struct ifreq) <= ic.len) {
            struct ifreq r;

            memset(&r, 0, sizeof(r));
            strcpy(r.name, d->name);
            sin_make(&r.u.sin, d->ip, 0);

            if ((err = copy_to_user(ic.buf + count * sizeof(r), &r, sizeof(r)))) break;
        } else if (ic.buf) {
            continue;                /* no room: the rest is not counted */
        }

        count++;
    }

    mutex_unlock(&net_lock);

    if (err) return err;

    ic.len = count * sizeof(struct ifreq);
    return copy_to_user(arg, &ic, sizeof(ic));
}

static long ifioctl(unsigned long cmd, unsigned long arg) {
    if (cmd == SIOCGIFCONF) return ifconf(arg);

    struct ifreq r;
    long err = copy_from_user(&r, arg, sizeof(r));

    if (err) return err;

    r.name[sizeof(r.name) - 1] = '\0';

    mutex_lock(&net_lock);

    struct netdev *d = NULL;

    if (cmd == SIOCGIFNAME) {
        int i = 1;

        for (d = netdev_first(); d && i < r.u.ivalue; d = d->next) i++;
        if (d) strcpy(r.name, d->name);
    } else {
        d = dev_by_name(r.name);
    }

    if (!d) {
        mutex_unlock(&net_lock);
        return -ENODEV;
    }

    err = 0;

    switch (cmd) {
        case SIOCGIFNAME:
            break;

        case SIOCGIFFLAGS: {
            uint16_t fl = 0;

            if (d->flags & NETDEV_UP) fl |= IFF_UP;
            if (d->flags & NETDEV_LOOPBACK) fl |= IFF_LOOPBACK;
            else fl |= IFF_BROADCAST | IFF_MULTICAST;
            if ((d->flags & NETDEV_UP) && (d->flags & NETDEV_LINK)) fl |= IFF_RUNNING;

            r.u.flags = fl;
            break;
        }

        case SIOCSIFFLAGS:
            if (r.u.flags & IFF_UP) d->flags |= NETDEV_UP;
            else d->flags &= ~NETDEV_UP;
            break;

        case SIOCGIFADDR:    sin_make(&r.u.sin, d->ip, 0); break;
        case SIOCGIFNETMASK: sin_make(&r.u.sin, d->mask, 0); break;
        case SIOCGIFBRDADDR: sin_make(&r.u.sin, d->ip | ~d->mask, 0); break;

        case SIOCSIFADDR:
            if (r.u.sin.family != AF_INET) {
                err = -EAFNOSUPPORT;
                break;
            }

            d->ip = ntohl(r.u.sin.addr);
            if (!d->mask) d->mask = default_mask(d->ip);
            break;

        case SIOCSIFNETMASK:
            d->mask = ntohl(r.u.sin.addr);
            break;

        case SIOCGIFMTU:
            r.u.ivalue = d->mtu;
            break;

        case SIOCGIFINDEX:
            r.u.ivalue = dev_index(d);
            break;

        case SIOCGIFHWADDR:
            memset(&r.u.hw, 0, sizeof(r.u.hw));
            r.u.hw.family = d->flags & NETDEV_LOOPBACK ? ARPHRD_LOOPBACK : ARPHRD_ETHER;
            memcpy(r.u.hw.data, d->mac, ETH_ALEN);
            break;

        default:
            err = -ENOTTY;
    }

    mutex_unlock(&net_lock);

    if (err) return err;

    return cmd == SIOCSIFFLAGS || cmd == SIOCSIFADDR || cmd == SIOCSIFNETMASK ? 0 : copy_to_user(arg, &r, sizeof(r));
}

static long inet_op_ioctl(struct inode *inode, unsigned long cmd, unsigned long arg) {
    struct isock *sk = inode->priv;
    int32_t n;

    switch (cmd) {
        case FIONREAD:
            mutex_lock(&net_lock);

            if (sk->type == SOCK_STREAM) n = sk->tcb->state == TCP_LISTEN ? 0 : sk->tcb->rx_bytes;
            else n = list_empty(&sk->rx) ? 0 : list_first(&sk->rx, struct nbuf, node)->len;

            mutex_unlock(&net_lock);
            return copy_to_user(arg, &n, sizeof(n));

        case SIOCOUTQ:
            mutex_lock(&net_lock);
            n = sk->type == SOCK_STREAM ? sk->tcb->snd_bytes : 0;
            mutex_unlock(&net_lock);
            return copy_to_user(arg, &n, sizeof(n));
    }

    if (cmd >= SIOCGIFNAME && cmd <= SIOCGIFINDEX) return ifioctl(cmd, arg);

    return -ENOTTY;
}

static void inet_op_release(struct inode *inode) {
    if (((struct isock *)inode->priv)->type == SOCK_STREAM) {
        tcp_op_release(inode);
        return;
    }

    mutex_lock(&net_lock);
    udp_unbind(inode->priv);
    mutex_unlock(&net_lock);
}

static void inet_op_put(struct sock *sk) {
    struct isock *isk = (struct isock *)sk;

    /* a socket that never had a file (accept could not make one) still holds its connection */
    if (isk->type == SOCK_STREAM && (isk->tcb || isk->bound)) tcp_sock_release(isk);

    kfree(sk);
}

const struct proto_ops inet_proto_ops = {
    .bind       = inet_op_bind,
    .connect    = inet_op_connect,
    .listen     = inet_op_listen,
    .accept     = inet_op_accept,
    .getname    = inet_op_getname,
    .shutdown   = inet_op_shutdown,
    .sendmsg    = inet_op_sendmsg,
    .recvmsg    = inet_op_recvmsg,
    .setsockopt = inet_op_setsockopt,
    .getsockopt = inet_op_getsockopt,
    .read       = inet_op_read,
    .write      = inet_op_write,
    .poll       = inet_op_poll,
    .ioctl      = inet_op_ioctl,
    .release    = inet_op_release,
    .put        = inet_op_put,
};
