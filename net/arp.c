#include <serika/cpu.h>
#include <serika/errno.h>
#include <serika/net.h>
#include <serika/string.h>
#include <serika/task.h>

#include <asm/arch.h>

/* ARP: who has this IPv4 address. 16 entries. an address that is not known has a request out, again each second, three
 * times, and the packets for it (up to three) wait in the entry; if nobody answers they are dropped */

#define ARP_ENTRIES 16
#define ARP_QUEUE   3
#define ARP_TRIES   3
#define ARP_VALID_S 300
#define ARP_RETRY_S 1

#define ARP_REQUEST 1
#define ARP_REPLY   2

struct arp_pkt {
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen, plen;
    uint16_t op;
    uint8_t  sha[ETH_ALEN];
    uint32_t spa;
    uint8_t  tha[ETH_ALEN];
    uint32_t tpa;
} __attribute__((packed));

enum { ARP_FREE, ARP_PENDING, ARP_VALID };

struct arp_entry {
    int            state;
    uint32_t       ip;
    uint8_t        mac[ETH_ALEN];
    struct netdev *dev;
    uint32_t       tries;
    uint64_t       when;          /* valid: until. pending: the next request */
    list_node      queue;
    int            queued;
};

static struct arp_entry table[ARP_ENTRIES];

static uint64_t seconds(uint32_t s) {
    return (uint64_t)s * this_cpu_read(tsc_freq_hz);
}

static void drop_queue(struct arp_entry *e) {
    while (!list_empty(&e->queue)) {
        struct nbuf *n = list_first(&e->queue, struct nbuf, node);

        list_del(&n->node);
        nbuf_free(n);
    }

    e->queued = 0;
}

static struct arp_entry *find(uint32_t ip) {
    for (int i = 0; i < ARP_ENTRIES; i++)
        if (table[i].state != ARP_FREE && table[i].ip == ip) return &table[i];

    return NULL;
}

/* a free entry, or the valid one that expires first. NULL: all have packets waiting */
static struct arp_entry *take(void) {
    struct arp_entry *best = NULL;

    for (int i = 0; i < ARP_ENTRIES; i++) {
        struct arp_entry *e = &table[i];

        if (e->state == ARP_FREE) {
            best = e;
            break;
        }

        if (e->state == ARP_VALID && (!best || e->when < best->when)) best = e;
    }

    if (best) {
        memset(best, 0, sizeof(*best));
        list_init(&best->queue);
    }

    return best;
}

static void send(struct netdev *dev, uint16_t op, const uint8_t *tha, uint32_t tpa) {
    struct nbuf *n = nbuf_alloc();
    if (!n) return;

    struct arp_pkt *p = nbuf_put(n, sizeof(*p));

    p->htype = htons(1);
    p->ptype = htons(ETH_P_IP);
    p->hlen  = ETH_ALEN;
    p->plen  = 4;
    p->op    = htons(op);

    memcpy(p->sha, dev->mac, ETH_ALEN);
    p->spa = htonl(dev->ip);

    if (op == ARP_REQUEST) memset(p->tha, 0, ETH_ALEN);
    else memcpy(p->tha, tha, ETH_ALEN);

    p->tpa = htonl(tpa);

    static const uint8_t all[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

    eth_tx(dev, op == ARP_REQUEST ? all : tha, ETH_P_ARP, n);
}

/* its address is known: what waited goes */
static void learned(struct arp_entry *e, const uint8_t *mac) {
    memcpy(e->mac, mac, ETH_ALEN);
    e->state = ARP_VALID;
    e->when  = rdtsc() + seconds(ARP_VALID_S);

    while (!list_empty(&e->queue)) {
        struct nbuf *n = list_first(&e->queue, struct nbuf, node);

        list_del(&n->node);
        eth_tx(e->dev, e->mac, ETH_P_IP, n);
    }

    e->queued = 0;
}

void arp_rx(struct netdev *dev, struct nbuf *n) {
    struct arp_pkt *p = nbuf_pull(n, sizeof(*p));

    if (!p || p->htype != htons(1) || p->ptype != htons(ETH_P_IP) || p->hlen != ETH_ALEN || p->plen != 4) {
        nbuf_free(n);
        return;
    }

    uint16_t op  = ntohs(p->op);
    uint32_t spa = ntohl(p->spa), tpa = ntohl(p->tpa);

    /* the sender is learned if we had it, or if it speaks to us */
    if (spa) {
        struct arp_entry *e = find(spa);

        if (!e && tpa == dev->ip) {
            e = take();
            if (e) {
                e->ip  = spa;
                e->dev = dev;
            }
        }

        if (e) learned(e, p->sha);
    }

    if (op == ARP_REQUEST && dev->ip && tpa == dev->ip) send(dev, ARP_REPLY, p->sha, spa);

    nbuf_free(n);
}

int arp_tx(struct netdev *dev, uint32_t nexthop, struct nbuf *n) {
    static const uint8_t all[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

    if (nexthop == 0xffffffffU || nexthop == (dev->ip | ~dev->mask)) return eth_tx(dev, all, ETH_P_IP, n);

    struct arp_entry *e = find(nexthop);

    if (e && e->state == ARP_VALID) return eth_tx(dev, e->mac, ETH_P_IP, n);

    if (!e) {
        e = take();

        if (!e) {
            nbuf_free(n);
            return -ENOBUFS;
        }

        e->state = ARP_PENDING;
        e->ip    = nexthop;
        e->dev   = dev;
        e->tries = 1;
        e->when  = rdtsc() + seconds(ARP_RETRY_S);

        send(dev, ARP_REQUEST, NULL, nexthop);
    }

    if (e->queued >= ARP_QUEUE) {
        nbuf_free(n);
        return -ENOBUFS;
    }

    list_add_tail(&n->node, &e->queue);
    e->queued++;
    return 0;
}

void arp_tick(void) {
    uint64_t now = rdtsc();

    for (int i = 0; i < ARP_ENTRIES; i++) {
        struct arp_entry *e = &table[i];

        if (e->state == ARP_FREE || now < e->when) continue;

        if (e->state == ARP_VALID) {
            e->state = ARP_FREE;
        } else if (e->tries >= ARP_TRIES) {
            drop_queue(e);
            e->state = ARP_FREE;
        } else {
            e->tries++;
            e->when = now + seconds(ARP_RETRY_S);
            send(e->dev, ARP_REQUEST, NULL, e->ip);
        }
    }
}

bool arp_lookup(uint32_t ip, uint8_t mac[ETH_ALEN]) {
    struct arp_entry *e = find(ip);

    if (!e || e->state != ARP_VALID) return false;

    memcpy(mac, e->mac, ETH_ALEN);
    return true;
}
