#include <serika/errno.h>
#include <serika/net.h>
#include <serika/string.h>

/* Ethernet II: a frame is the header of 14 bytes and the packet. the checksum at the end is the NIC's */

static const uint8_t broadcast[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

void eth_rx(struct netdev *dev, struct nbuf *n) {
    struct eth_hdr *h = nbuf_pull(n, ETH_HLEN);

    /* for us, or for everyone. multicast is not joined */
    if (!h || (memcmp(h->dst, dev->mac, ETH_ALEN) != 0 && memcmp(h->dst, broadcast, ETH_ALEN) != 0)) {
        nbuf_free(n);
        return;
    }

    switch (ntohs(h->type)) {
    case ETH_P_IP:  ip_rx(dev, n);  break;
    case ETH_P_ARP: arp_rx(dev, n); break;
    default:        nbuf_free(n);   break;
    }
}

int eth_tx(struct netdev *dev, const uint8_t *dst, uint16_t type, struct nbuf *n) {
    struct eth_hdr *h = nbuf_push(n, ETH_HLEN);

    if (!h) {
        nbuf_free(n);
        return -ENOBUFS;
    }

    memcpy(h->dst, dst, ETH_ALEN);
    memcpy(h->src, dev->mac, ETH_ALEN);
    h->type = htons(type);

    n->dev = dev;
    return dev->xmit(dev, n);
}
