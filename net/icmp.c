#include <serika/cpu.h>
#include <serika/errno.h>
#include <serika/net.h>
#include <serika/sched.h>
#include <serika/string.h>
#include <serika/task.h>

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

void icmp_rx(struct netdev *dev, uint32_t src, uint32_t dst, struct nbuf *n) {
    struct icmp_hdr *h = (struct icmp_hdr *)n->data;

    if (n->len < sizeof(*h) || net_csum(h, n->len, 0) != 0) goto drop;

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
