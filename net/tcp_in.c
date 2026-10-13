#include <serika/cpu.h>
#include <serika/errno.h>
#include <serika/net.h>
#include <serika/slab.h>
#include <serika/string.h>
#include <serika/tcp.h>

#include <asm/arch.h>

/* TCP, coming in: RFC 9293, 3.10.7 (segment arrives), with the checks of RFC 5961 for resets and SYNs, timestamps (PAWS), NewReno
 * (RFC 6582) and the rest of 5681 for the congestion window */

struct opts {
    uint16_t mss;
    int      ws;                   /* -1: none */
    bool     ts;
    uint32_t tsval, tsecr;
};

static uint32_t min32(uint32_t a, uint32_t b) {
    return a < b ? a : b;
}

static uint32_t max32(uint32_t a, uint32_t b) {
    return a > b ? a : b;
}

static uint64_t secs(uint32_t s) {
    return (uint64_t)s * this_cpu_read(tsc_freq_hz);
}

static void parse_opts(const uint8_t *p, unsigned len, struct opts *o) {
    o->mss = 0;
    o->ws  = -1;
    o->ts  = false;

    while (len) {
        uint8_t kind = p[0];

        if (kind == 0) break;

        if (kind == 1) {
            p++;
            len--;
            continue;
        }

        if (len < 2 || p[1] < 2 || p[1] > len) break;

        uint8_t l = p[1];

        if (kind == 2 && l == 4) o->mss = p[2] << 8 | p[3];
        if (kind == 3 && l == 3) o->ws = p[2] > 14 ? 14 : p[2];

        if (kind == 8 && l == 10) {
            o->ts    = true;
            o->tsval = (uint32_t)p[2] << 24 | p[3] << 16 | p[4] << 8 | p[5];
            o->tsecr = (uint32_t)p[6] << 24 | p[7] << 16 | p[8] << 8 | p[9];
        }

        p += l;
        len -= l;
    }
}

/* what a SYN says about the connection: the size of a segment, the scale, timestamps. active: this is the answer to ours */
static bool negotiate(struct tcb *tp, const struct opts *o, uint16_t win, bool active) {
    uint32_t mtu = ip_mtu_for(tp->raddr);

    if (mtu < 576) return false;                  /* no route */

    uint32_t peer = o->mss ? o->mss : TCP_DEFAULT_MSS;

    tp->mss = min32(min32(peer, mtu - 40), TCP_SEG_CAP);

    if (active) {
        if (o->ws >= 0) {
            tp->snd_scale = o->ws;
        } else {
            tp->rcv_scale = tp->snd_scale = 0;
            tp->wscale_offered = false;
        }

        tp->ts_ok = o->ts;
    } else {
        if (o->ws >= 0) {
            tp->wscale_offered = true;
            tp->snd_scale = o->ws;
            tp->rcv_scale = tcp_scale_for(tp->rcvbuf);
        }

        tp->ts_ok = o->ts;
    }

    if (tp->ts_ok) {
        tp->ts_recent = o->tsval;
        tp->mss -= 12;                            /* the option is in each segment */
    }

    tp->snd_wnd = win;                            /* the window of a SYN is not scaled */
    tp->cwnd    = TCP_INIT_CWND * tp->mss;
    return true;
}

void tcp_keepalive_start(struct tcb *tp) {
    if (!tp->keepalive) return;

    tp->last_rx   = rdtsc();
    tp->ka_probes = 0;
    tp->ka_at     = tp->last_rx + secs(tp->ka_idle);
    tcp_arm(tp->ka_at);
}

static void enter_time_wait(struct tcb *tp) {
    tp->state = TCP_TIME_WAIT;
    tp->rto_at = tp->persist_at = tp->delack_at = tp->ka_at = 0;
    tp->tw_at = rdtsc() + secs(2 * TCP_MSL_S);

    tcp_arm(tp->tw_at);
}

/* ---- data ---- */

static void rxq_add(struct tcb *tp, struct nbuf *n, bool *keep) {
    /* into the last buffer if it fits, so small writes do not each take a buffer */
    if (!list_empty(&tp->rxq)) {
        struct nbuf *tail = container_of(tp->rxq.prev, struct nbuf, node);

        if (tail->data + tail->len + n->len <= tail->buf + sizeof(tail->buf)) {
            memcpy(tail->data + tail->len, n->data, n->len);
            tail->len += n->len;
            return;
        }
    }

    list_add_tail(&n->node, &tp->rxq);
    *keep = true;
}

/* data after a gap in `ooo`, by sequence number */
static void ooo_add(struct tcb *tp, struct nbuf *n, uint32_t seq) {
    n->addr = seq;

    list_node *at = tp->ooo.prev;

    while (at != &tp->ooo && SEQ_GT(container_of(at, struct nbuf, node)->addr, seq)) at = at->prev;

    list_add(&n->node, at);
    tp->ooo_bytes += n->len;
}

/* what the gap let come: from `ooo` into the queue, as far as it is in order */
static void ooo_merge(struct tcb *tp) {
    while (!list_empty(&tp->ooo)) {
        struct nbuf *o = list_first(&tp->ooo, struct nbuf, node);

        if (SEQ_GT(o->addr, tp->rcv_nxt)) break;

        list_del(&o->node);
        tp->ooo_bytes -= o->len;

        uint32_t skip = tp->rcv_nxt - o->addr;

        if (skip >= o->len) {
            nbuf_free(o);
            continue;
        }

        nbuf_pull(o, skip);

        tp->rcv_nxt  += o->len;
        tp->rx_bytes += o->len;

        bool keep = false;

        rxq_add(tp, o, &keep);
        if (!keep) nbuf_free(o);
    }
}

/* ---- ACKs ---- */

#define ACK_OK   0
#define ACK_DROP 1                      /* not to be looked at more; the connection answers it */
#define ACK_GONE 2                      /* the connection is over, and tp is not to be touched */

/* the ACK of a segment of a connection that is made */
static int ack_input(struct tcb *tp, uint32_t seq, uint32_t ack, uint32_t win, uint32_t dlen, uint8_t flags, const struct opts *o) {
    if (SEQ_GT(ack, tp->snd_max)) {              /* of what was not sent */
        tp->ack_now = true;
        return ACK_DROP;
    }

    uint32_t wnd_new = win << tp->snd_scale;

    if (tp->ts_ok && o->ts && SEQ_LE(seq, tp->rcv_nxt)) tp->ts_recent = o->tsval;

    if (SEQ_LT(ack, tp->snd_una)) return ACK_OK; /* an old one */

    if (ack == tp->snd_una) {
        bool dup = dlen == 0 && !(flags & (TCP_SYN | TCP_FIN)) && wnd_new == tp->snd_wnd && tp->snd_una != tp->snd_max;

        if (SEQ_LT(tp->snd_wl1, seq) || (tp->snd_wl1 == seq && SEQ_LE(tp->snd_wl2, ack))) {
            tp->snd_wnd = wnd_new;
            tp->snd_wl1 = seq;
            tp->snd_wl2 = ack;

            if (wnd_new) tp->persist_at = 0;
        }

        if (!dup) return ACK_OK;

        tp->dupacks++;

        if (tp->dupacks == 3 && !tp->recovery && SEQ_GT(tp->snd_una, tp->recover)) {
            /* fast retransmit: the segment that the duplicates ask for, and recovery */
            uint32_t flight = tp->snd_max - tp->snd_una;

            tp->ssthresh = max32(flight / 2, 2 * tp->mss);
            tp->recover  = tp->snd_max;
            tp->recovery = true;
            tp->rtt_tsc  = 0;

            tcp_retransmit_head(tp);

            tp->cwnd = tp->ssthresh + 3 * tp->mss;
        } else if (tp->recovery && tp->dupacks > 3) {
            tp->cwnd += tp->mss;                 /* another segment has left the network */
        }

        return ACK_OK;
    }

    /* new data is acknowledged */
    uint32_t acked = ack - tp->snd_una;
    bool fin_acked = SEQ_GT(ack, tp->snd_end);
    uint32_t data = fin_acked ? acked - 1 : acked;

    /* the sample of the round trip: the timestamp that comes back, or the segment that was timed */
    if (tp->ts_ok && o->ts && o->tsecr) {
        uint32_t ms = tcp_ts_now() - o->tsecr;

        if (ms < 60000) tcp_rtt_sample(tp, ms);
    } else if (tp->rtt_tsc && SEQ_GE(ack, tp->rtt_seq)) {
        tcp_rtt_sample(tp, (rdtsc() - tp->rtt_tsc) * 1000 / this_cpu_read(tsc_freq_hz));
        tp->rtt_tsc = 0;
    }

    uint32_t left = data;

    while (left && !list_empty(&tp->sndq)) {
        struct tseg *s = list_first(&tp->sndq, struct tseg, node);

        if (s->len <= left) {
            left -= s->len;
            list_del(&s->node);
            kfree(s);
        } else {
            s->off += left;
            s->len -= left;
            s->seq += left;
            left = 0;
        }
    }

    tp->snd_bytes -= data;
    tp->snd_una    = ack;
    tp->retries    = 0;

    tcp_fix_cur(tp);

    if (SEQ_LT(tp->snd_wl1, seq) || (tp->snd_wl1 == seq && SEQ_LE(tp->snd_wl2, ack))) {
        tp->snd_wnd = wnd_new;
        tp->snd_wl1 = seq;
        tp->snd_wl2 = ack;

        if (wnd_new) tp->persist_at = 0;
    }

    if (tp->snd_una == tp->snd_max) tp->rto_at = 0;
    else tcp_start_rto(tp);

    if (tp->recovery) {
        if (SEQ_GE(ack, tp->recover)) {
            tp->cwnd     = min32(tp->ssthresh, max32(tp->snd_max - tp->snd_una, tp->mss) + tp->mss);
            tp->recovery = false;
            tp->dupacks  = 0;
        } else {
            /* a partial ACK: the next hole is sent, and the window comes down by what was acknowledged */
            tcp_retransmit_head(tp);

            tp->cwnd = tp->cwnd > data ? tp->cwnd - data : 0;

            if (data >= tp->mss) tp->cwnd += tp->mss;
            if (tp->cwnd < tp->mss) tp->cwnd = tp->mss;
        }
    } else {
        tp->dupacks = 0;

        if (tp->cwnd < tp->ssthresh) tp->cwnd += min32(data, tp->mss);
        else tp->cwnd += max32(1, (uint64_t)tp->mss * tp->mss / tp->cwnd);

        if (tp->cwnd > (1U << 30)) tp->cwnd = 1U << 30;
    }

    if (fin_acked) {
        switch (tp->state) {
        case TCP_FIN_WAIT1:
            tp->state = TCP_FIN_WAIT2;

            /* nobody will read what comes, and the peer may never end: not for ever */
            if (!tp->sk) {
                tp->tw_at = rdtsc() + secs(2 * TCP_MSL_S);
                tcp_arm(tp->tw_at);
            }
            break;

        case TCP_CLOSING:
            enter_time_wait(tp);
            break;

        case TCP_LAST_ACK:
            tcp_set_closed(tp, 0);
            return ACK_GONE;

        default:
            break;
        }
    }

    return ACK_OK;
}

/* ---- a segment ---- */

static void listen_input(struct tcb *lis, uint32_t src, uint32_t dst, const struct tcp_hdr *h, uint32_t seq, uint8_t flags,
                         uint16_t win, const struct opts *o, uint32_t seglen) {
    if (flags & TCP_RST) return;

    if (flags & TCP_ACK) {
        tcp_reply_rst(src, dst, h, seglen);
        return;
    }

    if (!(flags & TCP_SYN)) return;

    if (lis->nr_accept + lis->nr_syn > lis->backlog || lis->nr_syn >= TCP_SYN_QUEUE) return;

    struct tcb *c = tcb_new();
    if (!c) return;

    c->laddr  = dst;
    c->lport  = ntohs(h->dport);
    c->raddr  = src;
    c->rport  = ntohs(h->sport);
    c->parent = lis;
    c->state  = TCP_SYN_RECV;
    c->rcvbuf = lis->rcvbuf;
    c->sndbuf = lis->sndbuf;
    c->nodelay   = lis->nodelay;
    c->keepalive = lis->keepalive;
    c->ka_idle   = lis->ka_idle;
    c->ka_intvl  = lis->ka_intvl;
    c->ka_cnt    = lis->ka_cnt;

    c->irs     = seq;
    c->rcv_nxt = seq + 1;

    if (!negotiate(c, o, win, false)) {
        kfree(c);
        return;
    }

    c->iss = tcp_isn(c->laddr, c->lport, c->raddr, c->rport);
    c->snd_una = c->iss;
    c->snd_nxt = c->snd_max = c->snd_end = c->iss + 1;
    c->recover = c->iss;
    c->snd_wl1 = seq;
    c->rcv_adv = c->rcv_nxt + min32(c->rcvbuf, 65535);

    list_add_tail(&c->node, &tcp_conns);
    c->listed = true;
    lis->nr_syn++;

    tcp_send_syn(c);
    tcp_start_rto(c);
}

void tcp_rx(struct netdev *dev, const struct ip_hdr *iph, struct nbuf *n) {
    struct tcp_hdr *h = (struct tcp_hdr *)n->data;
    struct opts o;
    bool keep = false;

    if (n->len < TCP_HLEN) goto drop;

    uint32_t hlen = (h->off >> 4) * 4;

    if (hlen < TCP_HLEN || hlen > n->len) goto drop;

    uint32_t src = ntohl(iph->src), dst = ntohl(iph->dst);

    /* to us only: not a broadcast */
    if (!(dev->flags & NETDEV_LOOPBACK) && dst != dev->ip) goto drop;

    if (net_csum(h, n->len, net_pseudo_sum(src, dst, IPPROTO_TCP, n->len)) != 0) goto drop;

    uint32_t seq = ntohl(h->seq), ack = ntohl(h->ack);
    uint8_t flags = h->flags;
    uint16_t win = ntohs(h->win);
    uint16_t sport = ntohs(h->sport), dport = ntohs(h->dport);

    parse_opts((const uint8_t *)(h + 1), hlen - TCP_HLEN, &o);

    nbuf_pull(n, hlen);

    uint32_t dlen = n->len;
    uint32_t seglen = dlen + !!(flags & TCP_SYN) + !!(flags & TCP_FIN);

    struct tcb *tp;

lookup:
    tp = tcp_lookup(dst, dport, src, sport);

    if (!tp) {
        struct tcb *lis = tcp_find_listener(dst, dport);

        if (lis) listen_input(lis, src, dst, h, seq, flags, win, &o, seglen);
        else if (!(flags & TCP_RST)) tcp_reply_rst(src, dst, h, seglen);

        goto drop;
    }

    /* a new SYN for a connection that is only waiting out its end is a new connection (RFC 9293, 3.10.7.4) */
    if (tp->state == TCP_TIME_WAIT && (flags & TCP_SYN) && !(flags & (TCP_ACK | TCP_RST)) && SEQ_GT(seq, tp->rcv_nxt)) {
        tcp_set_closed(tp, 0);
        goto lookup;
    }

    if (tp->state == TCP_SYN_SENT) {
        if (flags & TCP_ACK) {
            if (SEQ_LE(ack, tp->iss) || SEQ_GT(ack, tp->snd_nxt)) {
                if (!(flags & TCP_RST)) tcp_reply_rst(src, dst, h, seglen);
                goto drop;
            }
        }

        if (flags & TCP_RST) {
            if (flags & TCP_ACK) tcp_set_closed(tp, -ECONNREFUSED);
            goto drop;
        }

        if (!(flags & TCP_SYN)) goto drop;

        tp->irs     = seq;
        tp->rcv_nxt = seq + 1;

        if (!negotiate(tp, &o, win, true)) {
            tcp_set_closed(tp, -ENETUNREACH);
            goto drop;
        }

        tp->rcv_adv = tp->rcv_nxt + min32(tp->rcvbuf, 65535);
        tp->snd_wl1 = seq;
        tp->snd_wl2 = ack;
        tp->retries = 0;

        if (flags & TCP_ACK) {
            tp->snd_una = ack;
            tp->rto_at  = 0;
            tp->rto_ms  = TCP_RTO_INIT_MS;
            tp->state   = TCP_ESTABLISHED;
            tp->ack_now = true;

            tcp_keepalive_start(tp);
            tcp_output(tp);
        } else {
            /* the SYNs crossed: both sides sent one */
            tp->state = TCP_SYN_RECV;
            tcp_send_syn(tp);
            tcp_start_rto(tp);
        }

        goto drop;
    }

    /* PAWS: a segment older than one that was seen is one of a connection that was before */
    if (tp->ts_ok && o.ts && !(flags & TCP_RST) && (int32_t)(o.tsval - tp->ts_recent) < 0) {
        tp->ack_now = true;
        tcp_output(tp);
        goto drop;
    }

    /* is it in the window */
    {
        uint32_t wnd = SEQ_GT(tp->rcv_adv, tp->rcv_nxt) ? tp->rcv_adv - tp->rcv_nxt : 0;
        bool ok;

        if (seglen == 0) {
            ok = wnd == 0 ? seq == tp->rcv_nxt : (SEQ_GE(seq, tp->rcv_nxt) && SEQ_LT(seq, tp->rcv_nxt + wnd));
        } else {
            uint32_t last = seq + seglen - 1;

            ok = wnd != 0 && ((SEQ_GE(seq, tp->rcv_nxt) && SEQ_LT(seq, tp->rcv_nxt + wnd)) ||
                              (SEQ_GE(last, tp->rcv_nxt) && SEQ_LT(last, tp->rcv_nxt + wnd)));
        }

        if (!ok) {
            if (!(flags & TCP_RST)) {
                tp->ack_now = true;
                tcp_output(tp);
            }

            goto drop;
        }
    }

    if (flags & TCP_RST) {
        /* only the one that is exactly next is believed; one in the window gets an ACK that the real peer answers (RFC 5961) */
        if (seq != tp->rcv_nxt) {
            tp->ack_now = true;
            tcp_output(tp);
            goto drop;
        }

        switch (tp->state) {
        case TCP_SYN_RECV:
            tcp_set_closed(tp, tp->parent ? 0 : -ECONNREFUSED);
            break;
        case TCP_CLOSING:
        case TCP_LAST_ACK:
        case TCP_TIME_WAIT:
            tcp_set_closed(tp, 0);
            break;
        default:
            tcp_set_closed(tp, -ECONNRESET);
            break;
        }

        goto drop;
    }

    if (flags & TCP_SYN) {                        /* a SYN in a connection: answered with an ACK, which is the peer's to take */
        tp->ack_now = true;
        tcp_output(tp);
        goto drop;
    }

    if (!(flags & TCP_ACK)) goto drop;

    if (tp->state == TCP_SYN_RECV) {
        if (!(SEQ_LT(tp->snd_una, ack) && SEQ_LE(ack, tp->snd_nxt))) {
            tcp_reply_rst(src, dst, h, seglen);
            goto drop;
        }

        tp->snd_una = ack;
        tp->state   = TCP_ESTABLISHED;
        tp->retries = 0;
        tp->rto_at  = 0;
        tp->rto_ms  = TCP_RTO_INIT_MS;
        tp->snd_wnd = (uint32_t)win << tp->snd_scale;
        tp->snd_wl1 = seq;
        tp->snd_wl2 = ack;

        if (tp->parent) {
            struct tcb *p = tp->parent;

            p->nr_syn--;
            list_add_tail(&tp->accept_node, &p->accept_q);
            p->nr_accept++;
            tp->in_accept = true;
        }

        tcp_keepalive_start(tp);
    } else {
        int r = ack_input(tp, seq, ack, win, dlen, flags, &o);

        if (r == ACK_DROP) tcp_output(tp);
        if (r != ACK_OK) goto drop;
    }

    if (tp->state == TCP_TIME_WAIT) {
        /* the other end's FIN again (the ACK was lost) is answered and the wait starts over. an ACK alone is not: if both ends
         * wait, they would answer each other for ever */
        if (dlen || (flags & TCP_FIN)) {
            tp->ack_now = true;
            tp->tw_at = rdtsc() + secs(2 * TCP_MSL_S);
            tcp_arm(tp->tw_at);
            tcp_output(tp);
        }

        goto drop;
    }

    /* the data */
    uint32_t fin_seq = seq + dlen;
    bool fin = flags & TCP_FIN;
    uint32_t dseq = seq;

    if (dlen && (tp->state == TCP_ESTABLISHED || tp->state == TCP_FIN_WAIT1 || tp->state == TCP_FIN_WAIT2)) {
        if (!tp->sk && !tp->parent) {
            /* the owner closed it, and data comes: nobody will read it */
            tcp_send_rst(tp);
            tcp_set_closed(tp, -ECONNRESET);
            goto drop;
        }

        if (SEQ_LT(dseq, tp->rcv_nxt)) {
            uint32_t skip = tp->rcv_nxt - dseq;

            if (skip >= dlen) {
                dlen = 0;
                tp->ack_now = true;
                tp->dup_ack = true;
            } else {
                nbuf_pull(n, skip);
                dlen -= skip;
                dseq = tp->rcv_nxt;
            }
        }

        if (dlen && SEQ_GT(dseq + dlen, tp->rcv_adv)) {
            uint32_t cut = dseq + dlen - tp->rcv_adv;

            dlen = cut >= dlen ? 0 : dlen - cut;
            n->len = dlen;
            fin = false;
        }

        if (dlen) {
            if (dseq == tp->rcv_nxt) {
                uint32_t had = tp->ooo_bytes;

                tp->rcv_nxt  += dlen;
                tp->rx_bytes += dlen;
                rxq_add(tp, n, &keep);

                ooo_merge(tp);

                /* every second segment is acknowledged at once, one alone after a moment; a gap that was filled at once too */
                if (++tp->unacked >= 2 || had) {
                    tp->ack_now = true;
                } else if (!tp->delack_at) {
                    tp->delack_at = rdtsc() + secs(TCP_DELACK_MS) / 1000;
                    tcp_arm(tp->delack_at);
                }
            } else if (tp->ooo_bytes + dlen + tp->rx_bytes <= (uint32_t)tp->rcvbuf) {
                ooo_add(tp, n, dseq);
                keep = true;
                tp->ack_now = true;
                tp->dup_ack = true;
            }
        }
    } else if (dlen) {
        tp->ack_now = true;                       /* data after the other end's FIN, or before the connection is made: not taken */
    }

    if (fin && fin_seq == tp->rcv_nxt) {
        switch (tp->state) {
        case TCP_ESTABLISHED:
            tp->state = TCP_CLOSE_WAIT;
            tp->rcv_nxt++;
            tp->fin_rcvd = true;
            break;
        case TCP_FIN_WAIT1:
            tp->state = TCP_CLOSING;
            tp->rcv_nxt++;
            tp->fin_rcvd = true;
            break;
        case TCP_FIN_WAIT2:
            tp->rcv_nxt++;
            tp->fin_rcvd = true;
            enter_time_wait(tp);
            break;
        default:
            break;
        }

        tp->ack_now = true;
    }

    if (tp->keepalive && tp->state != TCP_CLOSED) tcp_keepalive_start(tp);

    if (tp->state != TCP_CLOSED) tcp_output(tp);

drop:
    if (!keep) nbuf_free(n);
}

/* ICMP says a segment of ours did not get there: a connection that is not made is over; one that is made waits for its timer */
void tcp_icmp_err(uint32_t laddr, uint16_t lport, uint32_t raddr, uint16_t rport, int err) {
    struct tcb *tp = tcp_lookup(laddr, lport, raddr, rport);

    if (tp && (tp->state == TCP_SYN_SENT || tp->state == TCP_SYN_RECV)) tcp_set_closed(tp, err);
}
