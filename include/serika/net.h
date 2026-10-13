#pragma once

#include <serika/list.h>
#include <serika/mutex.h>
#include <serika/socket.h>
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
    uint32_t       addr;     /* a datagram waiting in a socket: whose it is (host order) */
    uint16_t       port;
    uint8_t        buf[NBUF_TOTAL - 48];
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

/* the same, and a signal ends it: -ERESTARTSYS */
#define net_wait_intr(cond, deadline)                           \
    ({                                                          \
        struct wait_entry __we = { 0 };                         \
        int __ret = 0;                                          \
                                                                \
        for (;;) {                                              \
            prepare_to_wait(&net_wq, &__we);                    \
            if (cond) break;                                    \
            if (signal_pending(current_task())) {               \
                __ret = -ERESTARTSYS;                           \
                break;                                          \
            }                                                   \
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
#define IPPROTO_UDP  17

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

uint32_t net_csum_add(const void *data, size_t len, uint32_t sum); /* the sum of the words, not folded: what a pseudo header is */
uint16_t net_csum(const void *data, size_t len, uint32_t sum);     /* the Internet checksum, in network order; sum: carried from before */

void ip_rx(struct netdev *dev, struct nbuf *n);

/* the address that ip_tx() would send from to dst. 0: no route */
uint32_t ip_src_for(uint32_t dst);
bool     ip_is_local(uint32_t ip);

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

/* an error for a packet that could not be delivered: type and code, and the packet's IP header and first 8 bytes. net_lock held */
#define ICMP_DEST_UNREACH 3
#define ICMP_PORT_UNREACH 3
void icmp_error(uint8_t type, uint8_t code, const struct ip_hdr *orig);

/* ---- UDP ---- */

struct udp_hdr {
    uint16_t sport, dport;
    uint16_t len;             /* header and data */
    uint16_t csum;
} __attribute__((packed));

#define UDP_HLEN   8
#define UDP_MAX    (1500 - IP_HLEN - UDP_HLEN)   /* a datagram is never cut in pieces */
#define UDP_RCVBUF 212992
#define UDP_PORT_FIRST 49152

/* an AF_INET socket. all of it is under net_lock, which is also what a waiter lets go of */
struct isock {
    struct sock sock;
    int         type;         /* SOCK_DGRAM */
    int         protocol;
    list_node   node;         /* the table of the protocol while it is bound */
    bool        bound, connected, rx_shut, tx_shut;
    uint32_t    laddr, raddr; /* host order. laddr 0: any */
    uint16_t    lport, rport;

    list_node   rx;           /* nbufs: addr and port are the sender's */
    size_t      rx_bytes;
    int         rcvbuf, sndbuf;
    uint64_t    rcvtimeo, sndtimeo; /* tsc ticks. 0: none */
    uint32_t    flags;        /* a bit for each option that is only given back: 1 << SO_x */
};

void udp_rx(struct netdev *dev, const struct ip_hdr *iph, struct nbuf *n);

/* net_lock held. the port 0 is any. -EADDRINUSE, -EADDRNOTAVAIL */
int  udp_bind(struct isock *sk, uint32_t addr, uint16_t port);
int  udp_send(struct isock *sk, uint32_t dst, uint16_t dport, const void *data, size_t len);
/* the next datagram into buf (cap bytes: the rest is lost, *full says how long it was); waits to the deadline unless
 * nonblock. the length copied, or -errno. 0 with peek leaves it queued */
long udp_recv(struct isock *sk, void *buf, size_t cap, size_t *full, uint32_t *saddr, uint16_t *sport, bool peek,
              bool nonblock, uint64_t deadline);
void udp_unbind(struct isock *sk);
uint32_t udp_poll(struct isock *sk);

/* ---- inet sockets (net/inet.c) ---- */

struct isock *isock_new(int type, int protocol);
struct sock  *inet_create(int type, int protocol, int *err);

/* the kernel's own use of UDP (DHCP). these take net_lock */
struct isock *udp_open(void);
void udp_close(struct isock *sk);
int  udp_bind_k(struct isock *sk, uint32_t addr, uint16_t port);
int  udp_send_k(struct isock *sk, uint32_t dst, uint16_t dport, const void *data, size_t len);
long udp_recv_k(struct isock *sk, void *buf, size_t cap, uint32_t *saddr, uint16_t *sport, uint32_t timeout_ms);

/* ---- drivers ---- */

void e1000e_init(void); /* an interface for each Intel 82574L, after netdev_init() */
