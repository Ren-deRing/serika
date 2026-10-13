#include <serika/cpu.h>
#include <serika/errno.h>
#include <serika/net.h>
#include <serika/sched.h>
#include <serika/signal.h>
#include <serika/slab.h>
#include <serika/string.h>
#include <serika/task.h>

#include <asm/arch.h>

/* UDP: a datagram is one IP packet (nothing is cut or put together), to a port. sockets are in one list while they are
 * bound, and a datagram goes to the one that fits best: a connected one before one that is only bound, one with an
 * address before one with any */

#define SO_REUSEADDR_BIT (1U << 2)

static list_node udp_socks = LIST_HEAD_INIT(udp_socks);

struct pseudo {
    uint32_t src, dst;        /* network order */
    uint8_t  zero, proto;
    uint16_t len;
} __attribute__((packed));

static uint32_t pseudo_sum(uint32_t src, uint32_t dst, uint16_t ulen) {
    struct pseudo ps = { .src = htonl(src), .dst = htonl(dst), .zero = 0, .proto = IPPROTO_UDP, .len = htons(ulen) };

    return net_csum_add(&ps, sizeof(ps), 0);
}

static bool overlap(uint32_t a, uint32_t b) {
    return a == 0 || b == 0 || a == b;
}

int udp_bind(struct isock *sk, uint32_t addr, uint16_t port) {
    static uint16_t next_port = UDP_PORT_FIRST;

    if (sk->bound) return -EINVAL;

    if (addr && !ip_is_local(addr) && (addr >> 24) != 127 && addr != 0xffffffffU) {
        /* the broadcast of a subnet */
        bool bcast = false;

        for (struct netdev *d = netdev_first(); d; d = d->next)
            if (d->ip && addr == (d->ip | ~d->mask)) bcast = true;

        if (!bcast) return -EADDRNOTAVAIL;
    }

    if (!port) {
        for (unsigned tries = 0; tries < 65536 - UDP_PORT_FIRST; tries++) {
            uint16_t p = next_port;

            next_port = next_port == 65535 ? UDP_PORT_FIRST : next_port + 1;

            bool used = false;

            for (list_node *l = udp_socks.next; l != &udp_socks; l = l->next)
                if (container_of(l, struct isock, node)->lport == p) used = true;

            if (!used) {
                port = p;
                break;
            }
        }

        if (!port) return -EADDRINUSE;
    } else {
        for (list_node *l = udp_socks.next; l != &udp_socks; l = l->next) {
            struct isock *o = container_of(l, struct isock, node);

            if (o->lport != port || !overlap(o->laddr, addr)) continue;

            /* both asked to share it */
            if ((o->flags & SO_REUSEADDR_BIT) && (sk->flags & SO_REUSEADDR_BIT)) continue;

            return -EADDRINUSE;
        }
    }

    sk->laddr = addr;
    sk->lport = port;
    sk->bound = true;
    list_add_tail(&sk->node, &udp_socks);
    return 0;
}

void udp_unbind(struct isock *sk) {
    if (sk->bound) list_del(&sk->node);

    sk->bound = false;

    while (!list_empty(&sk->rx)) {
        struct nbuf *n = list_first(&sk->rx, struct nbuf, node);

        list_del(&n->node);
        nbuf_free(n);
    }

    sk->rx_bytes = 0;
}

int udp_send(struct isock *sk, uint32_t dst, uint16_t dport, const void *data, size_t len) {
    if (len > UDP_MAX) return -EMSGSIZE;
    if (dport == 0) return -EINVAL;

    int err;

    if (!sk->bound && (err = udp_bind(sk, 0, 0))) return err;

    /* the broadcast of DHCP goes from no address at all */
    uint32_t from = ip_src_for(dst);
    if (!from && dst != 0xffffffffU) return -ENETUNREACH;

    uint32_t src = sk->laddr ? sk->laddr : from;

    struct nbuf *n = nbuf_alloc();
    if (!n) return -ENOMEM;

    struct udp_hdr *h = nbuf_put(n, UDP_HLEN + len);

    h->sport = htons(sk->lport);
    h->dport = htons(dport);
    h->len   = htons(UDP_HLEN + len);
    h->csum  = 0;

    memcpy(h + 1, data, len);

    h->csum = net_csum(h, UDP_HLEN + len, pseudo_sum(src, dst, UDP_HLEN + len));
    if (h->csum == 0) h->csum = 0xffff;             /* 0 says there is none */

    return ip_tx(src, dst, IPPROTO_UDP, n);
}

void udp_rx(struct netdev *dev, const struct ip_hdr *iph, struct nbuf *n) {
    struct udp_hdr *h = (struct udp_hdr *)n->data;

    if (n->len < UDP_HLEN) goto drop;

    uint32_t ulen = ntohs(h->len);

    if (ulen < UDP_HLEN || ulen > n->len) goto drop;

    n->len = ulen;

    uint32_t src = ntohl(iph->src), dst = ntohl(iph->dst);

    if (h->csum && net_csum(h, ulen, pseudo_sum(src, dst, ulen)) != 0) goto drop;

    uint16_t sport = ntohs(h->sport), dport = ntohs(h->dport);
    struct isock *best = NULL;
    int best_score = -1;

    for (list_node *l = udp_socks.next; l != &udp_socks; l = l->next) {
        struct isock *s = container_of(l, struct isock, node);

        if (s->lport != dport) continue;
        if (s->laddr && s->laddr != dst) continue;
        if (s->connected && (s->raddr != src || s->rport != sport)) continue;

        int score = (s->connected ? 2 : 0) + (s->laddr ? 1 : 0);

        if (score > best_score) {
            best = s;
            best_score = score;
        }
    }

    if (!best) {
        /* nobody there: told, if it was said to us and not to everyone */
        if (dst == dev->ip || (dev->flags & NETDEV_LOOPBACK)) icmp_error(ICMP_DEST_UNREACH, ICMP_PORT_UNREACH, iph);

        goto drop;
    }

    nbuf_pull(n, UDP_HLEN);

    if (best->rx_shut || best->rx_bytes + n->len > (size_t)best->rcvbuf) goto drop;

    n->addr = src;
    n->port = sport;

    list_add_tail(&n->node, &best->rx);
    best->rx_bytes += n->len;

    wake_up(&net_wq);
    return;

drop:
    nbuf_free(n);
}

long udp_recv(struct isock *sk, void *buf, size_t cap, size_t *full, uint32_t *saddr, uint16_t *sport, bool peek,
              bool nonblock, uint64_t deadline) {
    while (list_empty(&sk->rx)) {
        if (sk->rx_shut) return 0;
        if (nonblock) return -EAGAIN;

        int err = net_wait_intr(!list_empty(&sk->rx) || sk->rx_shut, deadline);

        if (err == -ETIMEDOUT) return -EAGAIN;
        if (err) return err;
    }

    struct nbuf *n = list_first(&sk->rx, struct nbuf, node);
    size_t copied = n->len < cap ? n->len : cap;

    memcpy(buf, n->data, copied);

    if (full)  *full  = n->len;
    if (saddr) *saddr = n->addr;
    if (sport) *sport = n->port;

    if (!peek) {
        list_del(&n->node);
        sk->rx_bytes -= n->len;
        nbuf_free(n);
    }

    return copied;
}

uint32_t udp_poll(struct isock *sk) {
    uint32_t mask = 0x004;       /* POLLOUT: a send does not wait */

    if (!list_empty(&sk->rx) || sk->rx_shut) mask |= 0x001;
    if (sk->rx_shut && sk->tx_shut) mask |= 0x010;

    return mask;
}

/* ---- the kernel's own sockets ---- */

struct isock *udp_open(void) {
    return isock_new(SOCK_DGRAM, IPPROTO_UDP);
}

void udp_close(struct isock *sk) {
    mutex_lock(&net_lock);
    udp_unbind(sk);
    mutex_unlock(&net_lock);

    kfree(sk);
}

int udp_bind_k(struct isock *sk, uint32_t addr, uint16_t port) {
    mutex_lock(&net_lock);
    int err = udp_bind(sk, addr, port);
    mutex_unlock(&net_lock);
    return err;
}

int udp_send_k(struct isock *sk, uint32_t dst, uint16_t dport, const void *data, size_t len) {
    mutex_lock(&net_lock);
    int err = udp_send(sk, dst, dport, data, len);
    mutex_unlock(&net_lock);
    return err;
}

long udp_recv_k(struct isock *sk, void *buf, size_t cap, uint32_t *saddr, uint16_t *sport, uint32_t timeout_ms) {
    size_t full;

    mutex_lock(&net_lock);
    long r = udp_recv(sk, buf, cap, &full, saddr, sport, false, false, net_ms_to_tsc(timeout_ms));
    mutex_unlock(&net_lock);

    return r == -EAGAIN ? -ETIMEDOUT : r;
}
