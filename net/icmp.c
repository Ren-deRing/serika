#include <serika/cpu.h>
#include <serika/errno.h>
#include <serika/net.h>
#include <serika/sched.h>
#include <serika/string.h>
#include <serika/task.h>
#include <serika/tcp.h>

#include <asm/arch.h>

/* ICMP: an echo request is answered, and icmp_ping() sends one and waits for the reply. one ping at a time */

#define PING_ID      0x5e11
#define PING_PAYLOAD 32

static struct {
    bool     active;
    bool     done;
    uint16_t seq;
    uint32_t dst;
    uint64_t rx_tsc;
} ping;

/* an ICMP error is about a packet of ours: its header and the first 8 bytes of its data are in the message. the socket
 * that sent it is told */
static void unreach_rx(uint8_t code, struct nbuf *n) {
    const struct ip_hdr *oh = (const struct ip_hdr *)(n->data + sizeof(struct icmp_hdr));
    uint32_t have = n->len - sizeof(struct icmp_hdr);

    if (have < IP_HLEN || (oh->ver_ihl >> 4) != 4) return;

    uint32_t hlen = (oh->ver_ihl & 0xf) * 4;

    if (hlen < IP_HLEN || have < hlen + 8) return;

    const uint8_t *t = (const uint8_t *)oh + hlen;
    uint32_t osrc = ntohl(oh->src), odst = ntohl(oh->dst);
    uint16_t sport = ntohs(*(const uint16_t *)t), dport = ntohs(*(const uint16_t *)(t + 2));
    int err;

    if (!ip_is_local(osrc) && (osrc >> 24) != 127) return;

    switch (code) {
    case 0:  err = -ENETUNREACH; break;
    case 2:
    case 3:  err = -ECONNREFUSED; break;
    case 4:  return;                       /* needs fragmenting: the path MTU is not looked for */
    default: err = -EHOSTUNREACH; break;
    }

    if (oh->proto == IPPROTO_UDP) udp_icmp_err(osrc, sport, odst, dport, err);
    else if (oh->proto == IPPROTO_TCP) tcp_icmp_err(osrc, sport, odst, dport, err);
}

void icmp_rx(struct netdev *dev, uint32_t src, uint32_t dst, struct nbuf *n) {
    struct icmp_hdr *h = (struct icmp_hdr *)n->data;

    if (n->len < sizeof(*h) || net_csum(h, n->len, 0) != 0) goto drop;

    if (h->type == ICMP_DEST_UNREACH) {
        unreach_rx(h->code, n);
        goto drop;
    }

    if (h->type == ICMP_ECHO_REQUEST) {
        /* to us and not to everyone (a broadcast would make us a part of a smurf) */
        if (!(dev->flags & NETDEV_LOOPBACK) && dst != dev->ip) goto drop;

        h->type = ICMP_ECHO_REPLY;
        h->csum = 0;
        h->csum = net_csum(h, n->len, 0);

        ip_tx(dst, src, IPPROTO_ICMP, n);
        return;
    }

    if (h->type == ICMP_ECHO_REPLY && ping.active && h->id == htons(PING_ID) && h->seq == htons(ping.seq) &&
        src == ping.dst) {
        ping.rx_tsc = rdtsc();
        ping.done   = true;
    }

drop:
    nbuf_free(n);
}

int icmp_ping(uint32_t dst, uint32_t timeout_ms, uint32_t *rtt_us) {
    uint64_t deadline = net_ms_to_tsc(timeout_ms);

    mutex_lock(&net_lock);

    int err = net_wait(!ping.active, deadline);

    if (err) goto out;

    struct nbuf *n = nbuf_alloc();

    if (!n) {
        err = -ENOMEM;
        goto out;
    }

    struct icmp_hdr *h = nbuf_put(n, sizeof(*h) + PING_PAYLOAD);

    h->type = ICMP_ECHO_REQUEST;
    h->code = 0;
    h->id   = htons(PING_ID);
    h->seq  = htons(++ping.seq);
    h->csum = 0;

    uint8_t *data = (uint8_t *)(h + 1);
    for (int i = 0; i < PING_PAYLOAD; i++) data[i] = i;

    h->csum = net_csum(h, sizeof(*h) + PING_PAYLOAD, 0);

    ping.dst    = dst;
    ping.done   = false;
    ping.active = true;

    uint64_t t0 = rdtsc();

    err = ip_tx(0, dst, IPPROTO_ICMP, n);

    if (!err) err = net_wait(ping.done, deadline);

    if (!err && rtt_us) *rtt_us = (ping.rx_tsc - t0) * 1000000 / this_cpu_read(tsc_freq_hz);

    ping.active = false;
    wake_up(&net_wq);

out:
    mutex_unlock(&net_lock);
    return err;
}

/* the packet that could not be delivered is told with its header and the first 8 bytes of its data */
void icmp_error(uint8_t type, uint8_t code, const struct ip_hdr *orig) {
    uint32_t hlen = (orig->ver_ihl & 0xf) * 4;
    uint32_t src = ntohl(orig->src);

    /* never to a broadcast or from a source that is not one host's, never about an ICMP error */
    if (src == 0 || src == 0xffffffffU || (src >> 28) == 0xe) return;
    if (orig->proto == IPPROTO_ICMP) return;

    struct nbuf *n = nbuf_alloc();
    if (!n) return;

    struct icmp_hdr *h = nbuf_put(n, sizeof(*h));

    h->type = type;
    h->code = code;
    h->csum = 0;
    h->id   = 0;      /* the unused word */
    h->seq  = 0;

    memcpy(nbuf_put(n, hlen + 8), orig, hlen + 8);

    h->csum = net_csum(h, n->len, 0);

    ip_tx(0, src, IPPROTO_ICMP, n);
}
