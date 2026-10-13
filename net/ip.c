#include <serika/errno.h>
#include <serika/net.h>
#include <serika/string.h>

/* IPv4. a packet that is not whole is dropped (a fragment too: none are put together, and none are sent, the MTU is 1500),
 * and what is not for us: no forwarding */

#define IP_TTL 64

uint32_t net_csum_add(const void *data, size_t len, uint32_t sum) {
    const uint8_t *p = data;

    /* words as they are in memory: the sum does not depend on the byte order, and comes out in the one of the packet */
    for (; len >= 2; len -= 2, p += 2) sum += *(const uint16_t *)p;

    if (len) sum += *p;

    return sum;
}

uint16_t net_csum(const void *data, size_t len, uint32_t sum) {
    sum = net_csum_add(data, len, sum);

    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);

    return (uint16_t)~sum;
}

bool ip_is_local(uint32_t ip) {
    for (struct netdev *d = netdev_first(); d; d = d->next)
        if (d->ip == ip) return true;

    return false;
}

static struct netdev *loopback(void) {
    for (struct netdev *d = netdev_first(); d; d = d->next)
        if (d->flags & NETDEV_LOOPBACK) return d;

    return NULL;
}

void ip_rx(struct netdev *dev, struct nbuf *n) {
    struct ip_hdr *h = (struct ip_hdr *)n->data;

    if (n->len < IP_HLEN || (h->ver_ihl >> 4) != 4) goto drop;

    uint32_t hlen = (h->ver_ihl & 0xf) * 4, total = ntohs(h->len);

    if (hlen < IP_HLEN || total < hlen || total > n->len) goto drop;
    if (net_csum(h, hlen, 0) != 0) goto drop;
    if (ntohs(h->frag) & 0x3fff) goto drop;      /* more fragments, or not the first */

    uint32_t src = ntohl(h->src), dst = ntohl(h->dst);

    /* the loopback carries what we sent to ourselves, to any of our addresses */
    if (!(dev->flags & NETDEV_LOOPBACK) && dst != dev->ip && dst != 0xffffffffU && dst != (dev->ip | ~dev->mask)) goto drop;

    n->len = total;                              /* the padding of a short frame is not the packet's */
    nbuf_pull(n, hlen);

    switch (h->proto) {
    case IPPROTO_ICMP: icmp_rx(dev, src, dst, n); return;
    case IPPROTO_UDP:  udp_rx(dev, h, n); return;
    default: break;
    }

drop:
    nbuf_free(n);
}

/* the way to dst: the interface, the next hop, and the address to send from. false: there is none */
static bool route(uint32_t dst, struct netdev **devp, uint32_t *nexthop, uint32_t *src) {
    struct netdev *dev = NULL;

    *nexthop = dst;

    if ((dst >> 24) == 127 || ip_is_local(dst)) {
        dev = loopback();

        /* from the address that was asked for: 127.0.0.1 to itself, an interface's own address to itself */
        *src = (dst >> 24) == 127 && dev ? dev->ip : dst;
    } else {
        /* on the link of an interface, or through the gateway of one */
        for (struct netdev *d = netdev_first(); d && !dev; d = d->next)
            if (!(d->flags & NETDEV_LOOPBACK) && d->ip && (dst & d->mask) == (d->ip & d->mask)) dev = d;

        for (struct netdev *d = netdev_first(); d && !dev; d = d->next) {
            if (!(d->flags & NETDEV_LOOPBACK) && d->gw) {
                dev = d;
                *nexthop = d->gw;
            }
        }

        if (dst == 0xffffffffU) {
            dev = NULL;

            for (struct netdev *d = netdev_first(); d && !dev; d = d->next)
                if (!(d->flags & NETDEV_LOOPBACK)) dev = d;

            *nexthop = dst;
        }

        *src = dev ? dev->ip : 0;
    }

    *devp = dev;
    return dev && (dev->flags & NETDEV_UP);
}

uint32_t ip_src_for(uint32_t dst) {
    struct netdev *dev;
    uint32_t nexthop, src;

    return route(dst, &dev, &nexthop, &src) ? src : 0;
}

int ip_tx(uint32_t src, uint32_t dst, uint8_t proto, struct nbuf *n) {
    static uint16_t next_id;

    struct netdev *dev;
    uint32_t nexthop, from;

    if (!route(dst, &dev, &nexthop, &from)) {
        nbuf_free(n);
        return -EHOSTUNREACH;
    }

    if (!src) src = from;

    struct ip_hdr *h = nbuf_push(n, IP_HLEN);

    if (!h || n->len > dev->mtu) {
        nbuf_free(n);
        return -EMSGSIZE;
    }

    h->ver_ihl = 0x45;
    h->tos     = 0;
    h->len     = htons(n->len);
    h->id      = htons(next_id++);
    h->frag    = htons(IP_DF);
    h->ttl     = IP_TTL;
    h->proto   = proto;
    h->csum    = 0;
    h->src     = htonl(src);
    h->dst     = htonl(dst);
    h->csum    = net_csum(h, IP_HLEN, 0);

    n->dev = dev;

    if (dev->flags & NETDEV_LOOPBACK) return dev->xmit(dev, n);

    return arp_tx(dev, nexthop, n);
}
