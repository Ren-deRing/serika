#include <serika/cpu.h>
#include <serika/errno.h>
#include <serika/fcntl.h>
#include <serika/net.h>
#include <serika/printk.h>
#include <serika/random.h>
#include <serika/sched.h>
#include <serika/string.h>
#include <serika/task.h>
#include <serika/timer.h>
#include <serika/vfs.h>

#include <asm/arch.h>

/* the DHCP client (RFC 2131) for one interface: DISCOVER, OFFER, REQUEST, ACK, and then the lease is kept. at T1 (half of
 * it) the server is asked to renew it, at T2 (7/8) anyone, and if the lease runs out the address is given up and it
 * begins again. the address the interface had before (the static one it is registered with) stays until there is a lease,
 * and stays if there is no server */

#define BOOTP_REQUEST 1
#define BOOTP_REPLY   2
#define BOOTP_BROADCAST 0x8000
#define DHCP_MAGIC    0x63825363U
#define PORT_SERVER   67
#define PORT_CLIENT   68

#define OPT_PAD       0
#define OPT_MASK      1
#define OPT_ROUTER    3
#define OPT_DNS       6
#define OPT_HOSTNAME  12
#define OPT_REQ_IP    50
#define OPT_LEASE     51
#define OPT_TYPE      53
#define OPT_SERVER    54
#define OPT_PARAMS    55
#define OPT_T1        58
#define OPT_T2        59
#define OPT_CLIENT_ID 61
#define OPT_END       255

#define DHCP_DISCOVER 1
#define DHCP_REQUEST  3

#define FIXED_LEN     236              /* the BOOTP header, up to the options */
#define INFINITE      0xffffffffU

#define RETRY_WAIT_S  30               /* with no server: again */
#define MIN_RETRY_S   60

struct bootp {
    uint8_t  op, htype, hlen, hops;
    uint32_t xid;                      /* network order from here on */
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t cookie;
    uint8_t  options[];
} __attribute__((packed));

static struct netdev *dev;
static struct isock  *sock;
static int            state;
static uint32_t       renewals;
static bool           forced;          /* net_lock */

int dhcp_state(void) {
    return state;
}

uint32_t dhcp_renewals(void) {
    return renewals;
}

void dhcp_renew_now(void) {
    mutex_lock(&net_lock);
    forced = true;
    wake_up(&net_wq);
    mutex_unlock(&net_lock);
}

/* ---- the messages ---- */

static size_t build(uint8_t *buf, int type, uint32_t xid, uint32_t ciaddr, uint32_t req_ip, uint32_t server, bool broadcast) {
    struct bootp *b = (struct bootp *)buf;

    memset(b, 0, sizeof(*b));

    b->op     = BOOTP_REQUEST;
    b->htype  = 1;
    b->hlen   = ETH_ALEN;
    b->xid    = htonl(xid);
    b->flags  = htons(broadcast ? BOOTP_BROADCAST : 0);
    b->ciaddr = htonl(ciaddr);
    b->cookie = htonl(DHCP_MAGIC);

    memcpy(b->chaddr, dev->mac, ETH_ALEN);

    uint8_t *o = b->options;

    *o++ = OPT_TYPE;      *o++ = 1; *o++ = type;
    *o++ = OPT_CLIENT_ID; *o++ = 7; *o++ = 1; memcpy(o, dev->mac, ETH_ALEN); o += ETH_ALEN;

    if (req_ip) { *o++ = OPT_REQ_IP; *o++ = 4; uint32_t v = htonl(req_ip); memcpy(o, &v, 4); o += 4; }
    if (server) { *o++ = OPT_SERVER; *o++ = 4; uint32_t v = htonl(server); memcpy(o, &v, 4); o += 4; }

    static const uint8_t params[] = { OPT_MASK, OPT_ROUTER, OPT_DNS, OPT_LEASE, OPT_T1, OPT_T2 };

    *o++ = OPT_PARAMS; *o++ = sizeof(params); memcpy(o, params, sizeof(params)); o += sizeof(params);
    *o++ = OPT_HOSTNAME; *o++ = 6; memcpy(o, "serika", 6); o += 6;
    *o++ = OPT_END;

    return o - buf;
}

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

int dhcp_parse(const void *msg, size_t len, uint32_t xid, const uint8_t mac[ETH_ALEN], int *type, struct dhcp_lease *l) {
    const struct bootp *b = msg;

    if (len < sizeof(*b) + 1 || b->op != BOOTP_REPLY || ntohl(b->xid) != xid || ntohl(b->cookie) != DHCP_MAGIC) return -EINVAL;
    if (b->htype != 1 || b->hlen != ETH_ALEN || memcmp(b->chaddr, mac, ETH_ALEN) != 0) return -EINVAL;

    memset(l, 0, sizeof(*l));
    l->ip = ntohl(b->yiaddr);
    *type = 0;

    const uint8_t *p = b->options, *end = (const uint8_t *)msg + len;

    while (p < end && *p != OPT_END) {
        if (*p == OPT_PAD) {
            p++;
            continue;
        }

        if (p + 2 > end || p + 2 + p[1] > end) return -EINVAL;

        uint8_t code = p[0], n = p[1];
        const uint8_t *v = p + 2;

        switch (code) {
        case OPT_TYPE:   if (n >= 1) *type = v[0]; break;
        case OPT_MASK:   if (n >= 4) l->mask = be32(v); break;
        case OPT_ROUTER: if (n >= 4) l->gw = be32(v); break;
        case OPT_DNS:    if (n >= 4) l->dns = be32(v); break;
        case OPT_SERVER: if (n >= 4) l->server = be32(v); break;
        case OPT_LEASE:  if (n >= 4) l->lease = be32(v); break;
        case OPT_T1:     if (n >= 4) l->t1 = be32(v); break;
        case OPT_T2:     if (n >= 4) l->t2 = be32(v); break;
        }

        p += 2 + n;
    }

    return *type ? 0 : -EINVAL;
}

void dhcp_times(struct dhcp_lease *l) {
    if (l->lease == 0) l->lease = 3600;             /* a server that says nothing */

    if (l->lease == INFINITE) {
        l->t1 = l->t2 = INFINITE;
        return;
    }

    /* T1 < T2 < the lease, or they are the halves and the 7/8 that the RFC says */
    if (!l->t1 || !l->t2 || l->t1 >= l->t2 || l->t2 >= l->lease) {
        l->t1 = l->lease / 2;
        l->t2 = l->lease / 8 * 7;
    }
}

/* ---- time ---- */

static uint64_t seconds(uint32_t s) {
    return (uint64_t)s * this_cpu_read(tsc_freq_hz);
}

/* wait until the deadline (~0: for ever). true if renew_now() was called */
static bool wait_until(uint64_t deadline) {
    mutex_lock(&net_lock);

    net_wait(forced, deadline);

    bool was = forced;

    forced = false;
    mutex_unlock(&net_lock);
    return was;
}

/* ---- the exchange ---- */

/* send, and wait up to timeout_ms for a message of this xid. 0 and what it says; -ETIMEDOUT */
static int transact(const uint8_t *msg, size_t len, uint32_t dst, uint32_t xid, uint32_t timeout_ms, int *type,
                    struct dhcp_lease *l) {
    static uint8_t buf[UDP_MAX];                  /* the one thread of dhcpd: not on its stack */

    if (udp_send_k(sock, dst, PORT_SERVER, msg, len) != 0) {
        /* no route yet, or the ARP queue is full: the wait below is the pause */
    }

    uint64_t deadline = net_ms_to_tsc(timeout_ms);

    for (;;) {
        uint64_t now = rdtsc();

        if (now >= deadline) return -ETIMEDOUT;

        uint32_t left = (deadline - now) * 1000 / this_cpu_read(tsc_freq_hz);
        long n = udp_recv_k(sock, buf, sizeof(buf), NULL, NULL, left ? left : 1);

        if (n < 0) return -ETIMEDOUT;

        if (dhcp_parse(buf, n, xid, dev->mac, type, l) == 0) return 0;
    }
}

static void apply(const struct dhcp_lease *l) {
    mutex_lock(&net_lock);

    dev->ip   = l->ip;
    dev->mask = l->mask ? l->mask : IP4(255, 255, 255, 0);
    dev->gw   = l->gw;
    dev->dns  = l->dns;

    mutex_unlock(&net_lock);
}

static void drop_address(void) {
    mutex_lock(&net_lock);

    dev->ip = dev->mask = dev->gw = dev->dns = 0;

    mutex_unlock(&net_lock);
}

/* /etc/resolv.conf: where glibc looks for the name server */
static void write_resolv(const struct dhcp_lease *l) {
    if (!l->dns) return;

    struct file *f;

    if (vfs_open("/etc/resolv.conf", O_WRONLY | O_CREAT | O_TRUNC, 0644, &f) != 0) return;

    char text[48];
    uint32_t d = l->dns;
    int n = 0;

    memcpy(text, "nameserver ", 11);
    n = 11;

    for (int i = 3; i >= 0; i--) {
        uint32_t v = (d >> (i * 8)) & 0xff;

        if (v >= 100) text[n++] = '0' + v / 100;
        if (v >= 10) text[n++] = '0' + v / 10 % 10;
        text[n++] = '0' + v % 10;
        if (i) text[n++] = '.';
    }

    text[n++] = '\n';

    uint64_t pos = 0;

    kernel_write(f, text, n, &pos);
    vfs_close(f);
}

/* DISCOVER ... ACK, up to four tries. 0 and the lease, or -ETIMEDOUT */
static int acquire(struct dhcp_lease *got) {
    static uint8_t msg[UDP_MAX];

    for (int attempt = 0; attempt < 4; attempt++) {
        struct dhcp_lease offer;
        uint32_t xid;
        int type;

        get_random_bytes(&xid, sizeof(xid));

        state = DHCP_SELECTING;

        size_t len = build(msg, DHCP_DISCOVER, xid, 0, 0, 0, true);

        if (transact(msg, len, 0xffffffffU, xid, 2000u << attempt, &type, &offer) != 0 || type != DHCP_OFFER) continue;

        state = DHCP_REQUESTING;

        len = build(msg, DHCP_REQUEST, xid, 0, offer.ip, offer.server, true);

        if (transact(msg, len, 0xffffffffU, xid, 4000, &type, got) != 0 || type != DHCP_ACK) continue;

        if (!got->ip) got->ip = offer.ip;
        if (!got->server) got->server = offer.server;

        dhcp_times(got);
        return 0;
    }

    return -ETIMEDOUT;
}

/* ask to keep the address, to the server (renewing) or to anyone (rebinding), until the time given. 0 and the new
 * lease; -ETIMEDOUT; -ECONNREFUSED: it was refused */
static int renew(const struct dhcp_lease *cur, bool rebinding, uint64_t until, struct dhcp_lease *got) {
    static uint8_t msg[UDP_MAX];

    for (;;) {
        uint64_t now = rdtsc();

        if (now >= until) return -ETIMEDOUT;

        /* the next try is half of what is left, but not sooner than a minute (or what is left) */
        uint64_t left = until - now, wait = left / 2;

        if (wait < seconds(MIN_RETRY_S)) wait = left < seconds(MIN_RETRY_S) ? left : seconds(MIN_RETRY_S);

        uint32_t xid, ms = wait * 1000 / this_cpu_read(tsc_freq_hz);
        int type;

        get_random_bytes(&xid, sizeof(xid));

        size_t len = build(msg, DHCP_REQUEST, xid, cur->ip, 0, 0, false);

        int err = transact(msg, len, rebinding ? 0xffffffffU : cur->server, xid, ms ? ms : 1, &type, got);

        if (err == 0 && type == DHCP_NAK) return -ECONNREFUSED;

        if (err == 0 && type == DHCP_ACK) {
            if (!got->ip) got->ip = cur->ip;
            if (!got->server) got->server = cur->server;
            if (!got->mask) got->mask = cur->mask;

            dhcp_times(got);
            return 0;
        }
    }
}

static void dhcpd(void) {
    struct dhcp_lease lease;

    sock = udp_open();

    if (!sock || udp_bind_k(sock, 0, PORT_CLIENT) != 0) {
        printk("dhcp: no socket\n");
        return;
    }

    /* the cable */
    for (int i = 0; i < 100 && !(dev->flags & NETDEV_LINK); i++) sleep_until(rdtsc() + seconds(1) / 10);

    for (;;) {
        state = DHCP_INIT;

        if (acquire(&lease) != 0) {
            printk("dhcp: no server, keeping %u.%u.%u.%u\n", dev->ip >> 24, dev->ip >> 16 & 255, dev->ip >> 8 & 255, dev->ip & 255);

            wait_until(rdtsc() + seconds(RETRY_WAIT_S));
            continue;
        }

        for (;;) {
            apply(&lease);
            write_resolv(&lease);

            state = DHCP_BOUND;
            printk("dhcp: %u.%u.%u.%u/%u.%u.%u.%u gw %u.%u.%u.%u from %u.%u.%u.%u, lease %us\n",
                   lease.ip >> 24, lease.ip >> 16 & 255, lease.ip >> 8 & 255, lease.ip & 255,
                   dev->mask >> 24, dev->mask >> 16 & 255, dev->mask >> 8 & 255, dev->mask & 255,
                   lease.gw >> 24, lease.gw >> 16 & 255, lease.gw >> 8 & 255, lease.gw & 255,
                   lease.server >> 24, lease.server >> 16 & 255, lease.server >> 8 & 255, lease.server & 255, lease.lease);

            uint64_t start = rdtsc();
            uint64_t t1 = lease.t1 == INFINITE ? ~0ULL : start + seconds(lease.t1);
            uint64_t t2 = lease.t2 == INFINITE ? ~0ULL : start + seconds(lease.t2);
            uint64_t end = lease.lease == INFINITE ? ~0ULL : start + seconds(lease.lease);

            wait_until(t1);

            struct dhcp_lease next;

            state = DHCP_RENEWING;
            int err = renew(&lease, false, t2 < end ? t2 : end, &next);

            if (err == -ETIMEDOUT) {
                state = DHCP_REBINDING;
                err = renew(&lease, true, end, &next);
            }

            if (err == 0) {
                lease = next;
                renewals++;
                continue;
            }

            break;                       /* refused, or the lease ran out */
        }

        printk("dhcp: lease lost\n");
        drop_address();
    }
}

void dhcp_init(void) {
    mutex_lock(&net_lock);

    for (struct netdev *d = netdev_first(); d; d = d->next) {
        if (d->flags & NETDEV_LOOPBACK) continue;

        dev = d;
        break;
    }

    mutex_unlock(&net_lock);

    if (!dev) return;

    struct task *t = task_create(dhcpd, 10);

    if (t) sched_add_task(t);
}
