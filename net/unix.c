#include <serika/buddy.h>
#include <serika/cpu.h>
#include <serika/errno.h>
#include <serika/poll.h>
#include <serika/preempt.h>
#include <serika/sched.h>
#include <serika/signal.h>
#include <serika/slab.h>
#include <serika/socket.h>
#include <serika/string.h>
#include <serika/syscall.h>
#include <serika/task.h>
#include <serika/timer.h>
#include <serika/vfs.h>

#include <asm/arch.h>

#define UNIX_RCVBUF 212992

/* every wait of every socket is on this one queue, and a wake-up only means look again. a queue per socket would have
 * to outlive the sockets that poll() and the senders still wait on; one that is never freed does not */
static struct wait_queue unix_wq;

static list_node  abstract_list;
static spinlock_t abstract_lock = SPINLOCK_INITIALIZER;
static uint32_t   autobind_next;

void net_init(void) {
    wait_queue_init(&unix_wq);
    list_init(&abstract_list);
}

/* ---- the sockets ---- */

struct usock *unix_create(int type) {
    struct usock *sk = kmalloc(sizeof(struct usock));
    if (!sk) return NULL;

    memset(sk, 0, sizeof(*sk));

    sk->sock.ops = &unix_proto_ops;
    sk->refs   = 1;
    sk->type   = type;
    sk->sndbuf = sk->rcvbuf = UNIX_RCVBUF;
    sk->cred   = (struct ucred){ .pid = sys_getpid(), .uid = 0, .gid = 0 };

    spin_lock_init(&sk->lock);
    list_init(&sk->rx);
    list_init(&sk->accept_q);
    list_init(&sk->abstract_node);
    list_init(&sk->accept_node);

    return sk;
}

void unix_get(struct usock *sk) {
    __atomic_add_fetch(&sk->refs, 1, 0); /* __ATOMIC_RELAXED */
}

void unix_put(struct usock *sk) {
    if (__atomic_sub_fetch(&sk->refs, 1, 4) == 0) kfree(sk); /* __ATOMIC_ACQ_REL */
}

/* ---- messages ---- */

static struct skb *skb_new(size_t len) {
    struct skb *skb = kmalloc(sizeof(struct skb));
    if (!skb) return NULL;

    memset(skb, 0, sizeof(*skb));
    skb->order = -1;

    if (len <= PAGE_SIZE) {
        if (len && !(skb->data = kmalloc(len))) {
            kfree(skb);
            return NULL;
        }
    } else {
        skb->order = SIZE_TO_ORDER(len);

        uintptr_t phys = (uintptr_t)alloc_pages(skb->order);
        if (!phys) {
            kfree(skb);
            return NULL;
        }

        skb->data = (uint8_t *)P2V(phys);
    }

    skb->len = len;
    return skb;
}

/* the files it carried are closed: never with a socket lock held */
static void skb_free(struct skb *skb) {
    for (unsigned i = 0; i < skb->nfds; i++) vfs_close(skb->fds[i]);

    kfree(skb->fds);

    if (skb->order >= 0) free_pages((void *)V2P((uintptr_t)skb->data), skb->order);
    else                 kfree(skb->data);

    kfree(skb);
}

static void drain(list_node *q) {
    while (!list_empty(q)) {
        struct skb *skb = list_first(q, struct skb, node);

        list_del(&skb->node);
        skb_free(skb);
    }
}

void unix_msg_drop(struct unix_msg *m) {
    for (unsigned i = 0; i < m->nfds; i++) vfs_close(m->fds[i]);
    m->nfds = 0;
}

void unix_rx_drop(struct unix_rx *rx) {
    for (unsigned i = 0; i < rx->nfds; i++) vfs_close(rx->fds[i]);
    rx->nfds = 0;
}

/* ---- waiting ---- */

struct unix_timeout {
    struct timer  t;
    volatile bool fired;
};

static void unix_timer_fn(struct timer *t) {
    struct unix_timeout *to = container_of(t, struct unix_timeout, t);

    to->fired = true;
    wake_up(&unix_wq);
}

/* until cond, a signal (-ERESTARTSYS), or the timeout (-EAGAIN). cond reads the sockets without their locks: it is only
 * ever looked at again under them */
#define unix_wait(cond, timeout)                                                    \
    ({                                                                              \
        struct unix_timeout __to = { .t = { .fn = unix_timer_fn } };                \
        uint64_t __ticks = (timeout);                                               \
                                                                                    \
        if (__ticks) {                                                              \
            __to.t.expires = rdtsc() + __ticks;                                     \
            timer_add(&__to.t);                                                     \
        }                                                                           \
                                                                                    \
        int __r = wait_event_interruptible(unix_wq, (cond) || __to.fired);          \
                                                                                    \
        if (__ticks) timer_del(&__to.t);                                            \
        __r ? __r : ((cond) ? 0 : -EAGAIN);                                         \
    })

static bool can_read(struct usock *sk) {
    return !list_empty(&sk->rx) || sk->rx_eof || sk->dead;
}

/* one message always fits an empty queue: one bigger than the buffer would wait for ever */
static bool has_room(struct usock *dst, size_t len) {
    return dst->rx_bytes == 0 || dst->rx_bytes + len <= (size_t)dst->rcvbuf;
}

/* ---- names ---- */

int unix_bind(struct usock *sk, const struct unix_addr *addr, bool abstract, bool autobind) {
    struct unix_addr a;
    int err = 0;

    if (autobind) {
        /* a free name: a nul and five hex digits */
        a.len = 6;
        a.name[0] = '\0';
    } else {
        a = *addr;
    }

    uint64_t flags = spin_lock_irqsave(&sk->lock);
    bool taken = sk->has_addr;
    spin_unlock_irqrestore(&sk->lock, flags);

    if (taken) return -EINVAL;

    if (abstract) {
        flags = spin_lock_irqsave(&abstract_lock);

        for (;;) {
            if (autobind) {
                static const char hex[] = "0123456789abcdef";
                uint32_t n = autobind_next++ & 0xfffff;

                for (int i = 0; i < 5; i++) a.name[1 + i] = hex[(n >> (16 - 4 * i)) & 0xf];
            }

            bool clash = false;

            for (list_node *it = abstract_list.next; it != &abstract_list; it = it->next) {
                struct usock *o = container_of(it, struct usock, abstract_node);

                if (o->addr.len == a.len && memcmp(o->addr.name, a.name, a.len) == 0) clash = true;
            }

            if (!clash || !autobind) {
                err = clash ? -EADDRINUSE : 0;
                break;
            }
        }

        if (!err) {
            sk->addr = a; /* abstract_lock first, then sk->lock, but nobody else looks until it is in the list */
            list_add_tail(&sk->abstract_node, &abstract_list);
        }

        spin_unlock_irqrestore(&abstract_lock, flags);
    } else {
        sk->addr = a;
    }

    if (err) return err;

    flags = spin_lock_irqsave(&sk->lock);
    sk->has_addr = true;
    sk->abstract = abstract;
    spin_unlock_irqrestore(&sk->lock, flags);

    return 0;
}

struct usock *unix_lookup_abstract(const struct unix_addr *addr) {
    struct usock *found = NULL;

    uint64_t flags = spin_lock_irqsave(&abstract_lock);

    for (list_node *it = abstract_list.next; it != &abstract_list; it = it->next) {
        struct usock *o = container_of(it, struct usock, abstract_node);

        if (o->addr.len == addr->len && memcmp(o->addr.name, addr->name, addr->len) == 0 && !o->dead) {
            unix_get(o);
            found = o;
            break;
        }
    }

    spin_unlock_irqrestore(&abstract_lock, flags);
    return found;
}

/* ---- connections ---- */

/* a socketpair: both ends connected, each seen by the other as the one that made it */
int unix_pair(struct usock *a, struct usock *b) {
    a->peer = b;
    b->peer = a;
    unix_get(a);
    unix_get(b);

    a->peercred = b->cred;
    b->peercred = a->cred;
    return 0;
}

int unix_listen(struct usock *sk, int backlog) {
    if (sk->type == SOCK_DGRAM) return -EOPNOTSUPP;

    uint64_t flags = spin_lock_irqsave(&sk->lock);

    int err = 0;

    if (sk->peer || sk->dead) {
        err = -EINVAL;
    } else {
        sk->listening = true;
        sk->backlog = backlog < 0 ? 0 : backlog > SOMAXCONN ? SOMAXCONN : backlog;
    }

    spin_unlock_irqrestore(&sk->lock, flags);
    return err;
}

/* a stream or seqpacket socket gets a partner made for it and put in the listener's queue. a datagram one only learns
 * where to send */
int unix_connect(struct usock *sk, struct usock *target, bool nonblock) {
    if (sk->type != target->type) return -EPROTOTYPE;

    if (sk->type == SOCK_DGRAM) {
        uint64_t flags = spin_lock_irqsave(&target->lock);
        bool dead = target->dead;
        spin_unlock_irqrestore(&target->lock, flags);

        if (dead) return -ECONNREFUSED;

        flags = spin_lock_irqsave(&sk->lock);

        struct usock *old = sk->peer;

        unix_get(target);
        sk->peer = target;
        sk->peer_closed = false;
        sk->tx_shut = false;

        spin_unlock_irqrestore(&sk->lock, flags);

        if (old) unix_put(old);
        return 0;
    }

    struct usock *srv = unix_create(sk->type);
    if (!srv) return -ENOMEM;

    unix_get(sk);
    srv->peer = sk;
    srv->peercred = sk->cred;

    uint64_t flags = spin_lock_irqsave(&sk->lock);

    int err = 0;

    if (sk->peer) err = -EISCONN;
    else if (sk->listening || sk->dead) err = -EINVAL;
    else {
        unix_get(srv);
        sk->peer = srv;
        sk->peer_closed = sk->rx_eof = sk->tx_shut = false;
    }

    spin_unlock_irqrestore(&sk->lock, flags);

    for (; !err;) {
        flags = spin_lock_irqsave(&target->lock);

        if (target->dead || !target->listening) {
            err = -ECONNREFUSED;
        } else if (target->nr_accept <= target->backlog) { /* linux counts one more than the backlog */
            srv->cred = target->cred;

            /* the new socket has the listener's name, which it does not hold in the abstract list: only the listener does */
            srv->has_addr = target->has_addr;
            srv->addr = target->addr;

            list_add_tail(&srv->accept_node, &target->accept_q);
            target->nr_accept++;
            spin_unlock_irqrestore(&target->lock, flags);

            flags = spin_lock_irqsave(&sk->lock);
            sk->peercred = target->cred;
            spin_unlock_irqrestore(&sk->lock, flags);

            wake_up(&unix_wq);
            return 0;
        }

        spin_unlock_irqrestore(&target->lock, flags);

        if (err) break;
        if (nonblock) {
            err = -EAGAIN;
            break;
        }

        err = unix_wait(target->dead || !target->listening || target->nr_accept <= target->backlog, 0);
    }

    /* not connected: the partner goes, and each reference to the other with it */
    flags = spin_lock_irqsave(&sk->lock);

    if (sk->peer == srv) {
        sk->peer = NULL;
        unix_put(srv);
    }

    spin_unlock_irqrestore(&sk->lock, flags);

    srv->peer = NULL;
    unix_put(sk);
    unix_put(srv);

    return err;
}

/* the partner made at connect time, with the queue's reference on it as ours now */
struct usock *unix_accept(struct usock *sk, bool nonblock, int *err) {
    *err = 0;

    for (;;) {
        uint64_t flags = spin_lock_irqsave(&sk->lock);

        if (!sk->listening) {
            spin_unlock_irqrestore(&sk->lock, flags);
            *err = -EINVAL;
            return NULL;
        }

        if (!list_empty(&sk->accept_q)) {
            struct usock *srv = list_first(&sk->accept_q, struct usock, accept_node);

            list_del(&srv->accept_node);
            sk->nr_accept--;

            spin_unlock_irqrestore(&sk->lock, flags);

            wake_up(&unix_wq);
            return srv;
        }

        spin_unlock_irqrestore(&sk->lock, flags);

        if (nonblock) {
            *err = -EAGAIN;
            return NULL;
        }

        *err = unix_wait(!list_empty(&sk->accept_q) || sk->dead || !sk->listening, sk->rcvtimeo);
        if (*err) return NULL;
    }
}

/* ---- data ---- */

long unix_send(struct usock *sk, struct unix_msg *m, bool nonblock) {
    bool dgram = sk->type == SOCK_DGRAM;
    struct usock *dst = NULL;
    struct unix_addr from;
    bool has_from;
    long err = 0;

    uint64_t flags = spin_lock_irqsave(&sk->lock);

    if (sk->tx_shut && !dgram) {
        err = -EPIPE;
    } else if (dgram && m->to) {
        dst = m->to;
    } else if (sk->peer) {
        dst = sk->peer;
    } else {
        err = dgram ? (sk->peer_closed ? -ECONNREFUSED : -ENOTCONN) : (sk->peer_closed ? -EPIPE : -ENOTCONN);
    }

    if (dst) unix_get(dst);

    from = sk->addr;
    has_from = sk->has_addr;

    spin_unlock_irqrestore(&sk->lock, flags);

    if (err) return err;

    if (dgram && dst->type != sk->type) {
        unix_put(dst);
        return -EPROTOTYPE;
    }

    if (!dgram && m->len == 0) {
        unix_put(dst);
        return 0;
    }

    struct skb *skb = skb_new(m->len);
    if (!skb) {
        unix_put(dst);
        return -ENOMEM;
    }

    memcpy(skb->data, m->data, m->len);

    if (m->nfds) {
        skb->fds = kmalloc(m->nfds * sizeof(struct file *));
        if (!skb->fds) {
            skb_free(skb);
            unix_put(dst);
            return -ENOMEM;
        }

        memcpy(skb->fds, m->fds, m->nfds * sizeof(struct file *));
        skb->nfds = m->nfds;
        m->nfds = 0; /* the message owns them now: if it is not queued, freeing it closes them */
    }

    skb->cred = m->has_cred ? m->cred : sk->cred;

    if (dgram && has_from) {
        skb->from = from;
        skb->has_from = true;
    }

    for (;;) {
        bool full = false;

        flags = spin_lock_irqsave(&dst->lock);

        if (dst->dead || (dgram && dst->peer && dst->peer != sk)) {
            err = dgram ? -ECONNREFUSED : -EPIPE;
        } else if (dst->rx_shut) {
            err = -EPIPE;
        } else if (has_room(dst, skb->len)) {
            list_add_tail(&skb->node, &dst->rx);
            dst->rx_bytes += skb->len;
            err = skb->len;
            skb = NULL;
        } else {
            full = true;
        }

        spin_unlock_irqrestore(&dst->lock, flags);

        if (!full) break;

        if (nonblock) {
            err = -EAGAIN;
            break;
        }

        err = unix_wait(dst->dead || dst->rx_shut || has_room(dst, skb->len), sk->sndtimeo);
        if (err) break;
    }

    unix_put(dst);

    if (skb) skb_free(skb);
    else     wake_up(&unix_wq);

    return err;
}

/* the first skb's worth for a datagram or seqpacket, as much as there is for a stream */
long unix_recv(struct usock *sk, struct unix_rx *rx, unsigned flags, bool nonblock) {
    bool peek = flags & MSG_PEEK;
    list_node done;

    list_init(&done);

    rx->len = rx->full = rx->nfds = 0;
    rx->has_cred = rx->has_from = rx->trunc = false;

    uint64_t f = spin_lock_irqsave(&sk->lock);

    for (;;) {
        if (!list_empty(&sk->rx)) break;

        if (sk->rx_eof) {
            spin_unlock_irqrestore(&sk->lock, f);
            return 0;
        }

        if (sk->type != SOCK_DGRAM && !sk->peer) {
            spin_unlock_irqrestore(&sk->lock, f);
            return -ENOTCONN;
        }

        spin_unlock_irqrestore(&sk->lock, f);

        if (nonblock) return -EAGAIN;

        int err = unix_wait(can_read(sk), sk->rcvtimeo);
        if (err) return err;

        f = spin_lock_irqsave(&sk->lock);
    }

    if (sk->type == SOCK_STREAM) {
        list_node *it = sk->rx.next;

        while (rx->len < rx->cap && it != &sk->rx) {
            struct skb *skb = container_of(it, struct skb, node);
            it = it->next;

            /* the files of a message belong to its first byte, and a sender's bytes are one read */
            if (rx->len > 0 && (skb->nfds || skb->cred.pid != rx->cred.pid)) break;

            size_t n = skb->len - skb->off;
            if (n > rx->cap - rx->len) n = rx->cap - rx->len;

            memcpy((uint8_t *)rx->buf + rx->len, skb->data + skb->off, n);

            if (!rx->len) {
                rx->cred = skb->cred;
                rx->has_cred = true;
            }

            rx->len += n;

            if (peek) continue;

            skb->off += n;
            sk->rx_bytes -= n;

            if (skb->nfds) {
                for (unsigned i = 0; i < skb->nfds; i++) rx->fds[rx->nfds++] = skb->fds[i];

                kfree(skb->fds);
                skb->fds = NULL;
                skb->nfds = 0;
            }

            if (skb->off == skb->len) {
                list_del(&skb->node);
                list_add_tail(&skb->node, &done);
            }
        }

        rx->full = rx->len;
    } else {
        struct skb *skb = list_first(&sk->rx, struct skb, node);
        size_t n = skb->len < rx->cap ? skb->len : rx->cap;

        memcpy(rx->buf, skb->data, n);

        rx->len = n;
        rx->full = skb->len;
        rx->trunc = skb->len > rx->cap;
        rx->cred = skb->cred;
        rx->has_cred = true;

        if (skb->has_from) {
            rx->from = skb->from;
            rx->has_from = true;
        }

        if (!peek) {
            for (unsigned i = 0; i < skb->nfds; i++) rx->fds[rx->nfds++] = skb->fds[i];

            kfree(skb->fds);
            skb->fds = NULL;
            skb->nfds = 0;

            sk->rx_bytes -= skb->len;

            list_del(&skb->node);
            list_add_tail(&skb->node, &done);
        }
    }

    spin_unlock_irqrestore(&sk->lock, f);

    drain(&done);
    wake_up(&unix_wq); /* a sender may be waiting for the room */

    return rx->len;
}

/* what is to be read: all the bytes of a stream, the size of the next datagram */
long unix_inq(struct usock *sk) {
    uint64_t f = spin_lock_irqsave(&sk->lock);

    long n = sk->rx_bytes;

    if (sk->type != SOCK_STREAM && !list_empty(&sk->rx)) n = list_first(&sk->rx, struct skb, node)->len;

    spin_unlock_irqrestore(&sk->lock, f);
    return n;
}

/* what the peer has not read yet */
long unix_outq(struct usock *sk) {
    uint64_t f = spin_lock_irqsave(&sk->lock);
    struct usock *peer = sk->peer;

    if (peer) unix_get(peer);

    spin_unlock_irqrestore(&sk->lock, f);

    if (!peer) return 0;

    f = spin_lock_irqsave(&peer->lock);
    long n = peer->rx_bytes;
    spin_unlock_irqrestore(&peer->lock, f);

    unix_put(peer);
    return n;
}

int unix_shutdown(struct usock *sk, int how) {
    struct usock *peer;
    bool rd = how == SHUT_RD || how == SHUT_RDWR, wr = how == SHUT_WR || how == SHUT_RDWR;

    uint64_t f = spin_lock_irqsave(&sk->lock);

    if (sk->type != SOCK_DGRAM && !sk->peer && !sk->peer_closed) {
        spin_unlock_irqrestore(&sk->lock, f);
        return -ENOTCONN;
    }

    if (rd) sk->rx_shut = sk->rx_eof = true;
    if (wr) sk->tx_shut = true;

    peer = sk->peer;
    if (peer) unix_get(peer);

    spin_unlock_irqrestore(&sk->lock, f);

    /* the other side reads the end of the stream */
    if (peer) {
        if (wr) {
            f = spin_lock_irqsave(&peer->lock);
            peer->rx_eof = true;
            spin_unlock_irqrestore(&peer->lock, f);
        }

        unix_put(peer);
    }

    wake_up(&unix_wq);
    return 0;
}

/* the last file of the socket is closed: the end for the other side, and everything unread goes */
void unix_release(struct usock *sk) {
    list_node rxq, aq;

    list_init(&rxq);
    list_init(&aq);

    uint64_t f = spin_lock_irqsave(&sk->lock);

    if (sk->dead) {
        spin_unlock_irqrestore(&sk->lock, f);
        return;
    }

    sk->dead = true;
    sk->listening = false;
    sk->rx_eof = sk->tx_shut = true;

    struct usock *peer = sk->peer;
    sk->peer = NULL;

    while (!list_empty(&sk->rx)) {
        struct skb *skb = list_first(&sk->rx, struct skb, node);

        list_del(&skb->node);
        list_add_tail(&skb->node, &rxq);
    }

    sk->rx_bytes = 0;

    while (!list_empty(&sk->accept_q)) {
        struct usock *srv = list_first(&sk->accept_q, struct usock, accept_node);

        list_del(&srv->accept_node);
        list_add_tail(&srv->accept_node, &aq);
    }

    sk->nr_accept = 0;

    spin_unlock_irqrestore(&sk->lock, f);

    drain(&rxq);

    /* those that were never accepted are closed on the connecting side's behalf */
    while (!list_empty(&aq)) {
        struct usock *srv = list_first(&aq, struct usock, accept_node);

        list_del(&srv->accept_node);
        unix_release(srv);
        unix_put(srv);
    }

    if (peer) {
        bool drop = false;

        f = spin_lock_irqsave(&peer->lock);

        if (peer->peer == sk) {
            peer->peer = NULL;
            peer->peer_closed = true;
            peer->rx_eof = peer->tx_shut = true;
            drop = true;
        }

        spin_unlock_irqrestore(&peer->lock, f);

        if (drop) unix_put(sk); /* the peer's reference on us */
        unix_put(peer);         /* ours on it */
    }

    if (sk->has_addr && sk->abstract) {
        f = spin_lock_irqsave(&abstract_lock);
        list_del(&sk->abstract_node);
        list_init(&sk->abstract_node);
        spin_unlock_irqrestore(&abstract_lock, f);
    }

    wake_up(&unix_wq);
}

uint32_t unix_poll(struct usock *sk, struct poll_table *pt) {
    uint32_t mask = 0;

    poll_wait(pt, &unix_wq);

    uint64_t f = spin_lock_irqsave(&sk->lock);

    struct usock *peer = sk->peer;
    bool connected = peer != NULL, listening = sk->listening;

    if (peer) unix_get(peer);

    if (listening) {
        if (!list_empty(&sk->accept_q)) mask |= POLLIN;
    } else {
        if (!list_empty(&sk->rx)) mask |= POLLIN;
        if (sk->rx_eof) mask |= POLLIN | 0x2000; /* POLLRDHUP, whatever is still to read */
        if (sk->rx_eof && sk->tx_shut) mask |= POLLHUP;
    }

    bool tx_shut = sk->tx_shut;
    int type = sk->type, sndbuf = sk->sndbuf;

    spin_unlock_irqrestore(&sk->lock, f);

    /* writable: a peer with room, or a datagram socket with no peer at all */
    if (!listening && !tx_shut) {
        if (!connected) {
            if (type == SOCK_DGRAM) mask |= POLLOUT;
        } else {
            /* writable when what is queued at the peer is a quarter of the send buffer or less, as linux says it: a
             * writer woken for every byte a reader frees would do a send of one for each */
            f = spin_lock_irqsave(&peer->lock);
            if (!peer->dead && !peer->rx_shut && peer->rx_bytes <= (size_t)sndbuf / 4) mask |= POLLOUT;
            spin_unlock_irqrestore(&peer->lock, f);
        }
    }

    if (peer) unix_put(peer);
    return mask;
}
