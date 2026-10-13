#pragma once

#include <serika/list.h>
#include <serika/mutex.h>
#include <serika/wait.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* the network stack: network interfaces, packets, Ethernet, ARP, IPv4 and ICMP. what the sockets of the internet
 * (UDP, TCP) are built on is in the sub-projects after this one. the unix sockets are socket.h */

static inline uint16_t htons(uint16_t v) { return __builtin_bswap16(v); }
static inline uint16_t ntohs(uint16_t v) { return __builtin_bswap16(v); }
static inline uint32_t htonl(uint32_t v) { return __builtin_bswap32(v); }
static inline uint32_t ntohl(uint32_t v) { return __builtin_bswap32(v); }

/* an address in host byte order: 10.0.2.15 is IP4(10, 0, 2, 15) */
#define IP4(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

/* ---- packets ---- */

/* a struct nbuf is one kmalloc object of 2 KiB: room in front for the headers that are put on as the packet goes down,
 * and for a frame (1514) behind them */
#define NBUF_TOTAL    2048
#define NBUF_HEADROOM 64

struct netdev;

struct nbuf {
    list_node      node;
    struct netdev *dev;      /* the interface it came in on or goes out of */
    uint8_t       *data;     /* the first byte of the packet as it is now */
    uint32_t       len;
    uint8_t        buf[NBUF_TOTAL - 40];
};

_Static_assert(sizeof(struct nbuf) == NBUF_TOTAL, "an nbuf is one 2 KiB object");

struct nbuf *nbuf_alloc(void);          /* empty, with the headroom. NULL: no memory */
void         nbuf_free(struct nbuf *n);
void        *nbuf_put(struct nbuf *n, uint32_t len);  /* len more bytes at the tail. NULL: no room */
void        *nbuf_push(struct nbuf *n, uint32_t len); /* room for a header in front. NULL: no room */
void        *nbuf_pull(struct nbuf *n, uint32_t len); /* a header taken off, the pointer to it. NULL: not so long */

/* ---- interfaces ---- */

#define NETDEV_UP       (1U << 0)
#define NETDEV_LOOPBACK (1U << 1)   /* no Ethernet: a packet is an IP packet from the start */
#define NETDEV_LINK     (1U << 2)   /* the cable is in */

struct netdev {
    char     name[16];
    uint8_t  mac[6];
    uint16_t mtu;
    uint32_t flags;
    uint32_t ip, mask, gw;           /* host byte order. 0: none */

    /* takes the nbuf and frees it, sent or not. net_lock is held. 0 or -errno */
    int  (*xmit)(struct netdev *dev, struct nbuf *n);

    /* empties the receive ring into net_rx(). from netd, net_lock held */
    void (*poll)(struct netdev *dev);

    void          *priv;
    uint32_t       pending;          /* net_schedule(): poll() has work */
    struct netdev *next;
};

/* all of the state of the stack is under this lock: netd holds it while it works, and whoever sends. a wait lets it go */
extern struct mutex net_lock;

void netdev_init(void);                      /* after pci_init(): the loopback, and the thread */
int  netdev_register(struct netdev *dev);    /* the first that is not the loopback gets 10.0.2.15/24 via 10.0.2.2 */
struct netdev *netdev_find(const char *name);
struct netdev *netdev_first(void);           /* the list, in the order of registration: next. net_lock held */

void net_schedule(struct netdev *dev);       /* from an interrupt: wake netd to poll the interface */
void net_rx(struct netdev *dev, struct nbuf *n); /* from poll(): one frame (a packet on the loopback) into the stack. takes it */

/* waits, with net_lock held on the way in and out, until cond holds (-ETIMEDOUT: not by the deadline, in tsc ticks). cond
 * is looked at with the lock; what changes it wakes net_wq (netd does after each round, and its tick, every 100 ms) */
extern struct wait_queue net_wq;

#define net_wait(cond, deadline)                                \
    ({                                                          \
        struct wait_entry __we = { 0 };                         \
        int __ret = 0;                                          \
                                                                \
        for (;;) {                                              \
            prepare_to_wait(&net_wq, &__we);                    \
            if (cond) break;                                    \
            if (rdtsc() >= (deadline)) {                        \
                __ret = -ETIMEDOUT;                             \
                break;                                          \
            }                                                   \
            mutex_unlock(&net_lock);                            \
            schedule();                                         \
            mutex_lock(&net_lock);                              \
        }                                                       \
                                                                \
        finish_wait(&net_wq, &__we);                            \
        __ret;                                                  \
    })

uint64_t net_ms_to_tsc(uint32_t ms);         /* from now */

/* ---- Ethernet ---- */

#define ETH_ALEN   6
#define ETH_HLEN   14
#define ETH_P_IP   0x0800
#define ETH_P_ARP  0x0806

struct eth_hdr {
    uint8_t  dst[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t type;            /* network order */
} __attribute__((packed));

void eth_rx(struct netdev *dev, struct nbuf *n);
int  eth_tx(struct netdev *dev, const uint8_t *dst, uint16_t type, struct nbuf *n); /* type: host order. takes n */

/* ---- ARP ---- */

void arp_rx(struct netdev *dev, struct nbuf *n);
int  arp_tx(struct netdev *dev, uint32_t nexthop, struct nbuf *n); /* an IP packet to the next hop; waits in a queue until its address is known. takes n */
void arp_tick(void);                          /* netd: requests again, gives up, forgets */
bool arp_lookup(uint32_t ip, uint8_t mac[ETH_ALEN]); /* only a known one */

/* ---- IPv4 ---- */

#define IPPROTO_ICMP 1

struct ip_hdr {
    uint8_t  ver_ihl;
    uint8_t  tos;
    uint16_t len;             /* header and data */
    uint16_t id;
    uint16_t frag;            /* flags and offset */
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t csum;
    uint32_t src, dst;        /* network order */
} __attribute__((packed));

#define IP_HLEN 20
#define IP_DF   0x4000

uint16_t net_csum(const void *data, size_t len, uint32_t sum); /* the Internet checksum, in network order; sum: carried from before */

void ip_rx(struct netdev *dev, struct nbuf *n);

/* src 0: that of the interface the route is through. n is the payload; takes it. -EHOSTUNREACH: no route */
int ip_tx(uint32_t src, uint32_t dst, uint8_t proto, struct nbuf *n);

/* ---- ICMP ---- */

struct icmp_hdr {
    uint8_t  type;
    uint8_t  code;
    uint16_t csum;
    uint16_t id;              /* of an echo */
    uint16_t seq;
} __attribute__((packed));

#define ICMP_ECHO_REPLY   0
#define ICMP_ECHO_REQUEST 8

void icmp_rx(struct netdev *dev, uint32_t src, uint32_t dst, struct nbuf *n);

/* an echo request and its reply. 0 and the round trip in microseconds, or -ETIMEDOUT, -EHOSTUNREACH. takes net_lock, may sleep */
int icmp_ping(uint32_t dst, uint32_t timeout_ms, uint32_t *rtt_us);

/* ---- drivers ---- */

void e1000e_init(void); /* an interface for each Intel 82574L, after netdev_init() */
