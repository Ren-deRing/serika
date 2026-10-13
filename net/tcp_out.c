#include <serika/cpu.h>
#include <serika/errno.h>
#include <serika/net.h>
#include <serika/slab.h>
#include <serika/string.h>
#include <serika/tcp.h>

#include <asm/arch.h>

/* TCP, going out: segments are built, the data that may go does, and the timer of a lost segment fires */

static uint64_t ms_to_tsc(uint32_t ms) {
    return (uint64_t)ms * this_cpu_read(tsc_freq_hz) / 1000;
}

static uint32_t min32(uint32_t a, uint32_t b) {
    return a < b ? a : b;
}

static uint32_t max32(uint32_t a, uint32_t b) {
    return a > b ? a : b;
}

void tcp_start_rto(struct tcb *tp) {
    tp->rto_at = rdtsc() + ms_to_tsc(tp->rto_ms);
    tcp_arm(tp->rto_at);
}

/* the window to say: what is free, but not moved by less than a segment or half of the buffer (a sender that has to
 * send small pieces is not told to), and never taken back */
uint32_t tcp_window(struct tcb *tp) {
    int free = tp->rcvbuf - (int)tp->rx_bytes - (int)tp->ooo_bytes;

    if (free < 0) free = 0;

    uint32_t edge = tp->rcv_nxt + free;

    if (!tp->dup_ack && SEQ_GT(edge, tp->rcv_adv) && edge - tp->rcv_adv >= min32(tp->mss, tp->rcvbuf / 2)) tp->rcv_adv = edge;

    if (SEQ_LT(tp->rcv_adv, tp->rcv_nxt)) tp->rcv_adv = tp->rcv_nxt;

    uint32_t win = (tp->rcv_adv - tp->rcv_nxt) >> tp->rcv_scale;

    return win > 65535 ? 65535 : win;
}

/* a segment: flags, and len bytes of data at seq. the options are those of a SYN, or the timestamps */
static int xmit(struct tcb *tp, uint32_t seq, uint8_t flags, const uint8_t *data, uint32_t len) {
    uint8_t opts[40];
    unsigned olen = 0;
    bool syn = flags & TCP_SYN, rst = flags & TCP_RST;
    uint16_t win;

    if (syn) {
        /* the MSS of the MTU, then (we ask in a SYN, and answer what the peer asked for in a SYN+ACK) the scale and timestamps */
        opts[olen++] = 2; opts[olen++] = 4; opts[olen++] = TCP_SEG_CAP >> 8; opts[olen++] = TCP_SEG_CAP & 0xff;

        if (tp->state == TCP_SYN_SENT || tp->wscale_offered) {
            opts[olen++] = 1; opts[olen++] = 3; opts[olen++] = 3; opts[olen++] = tp->rcv_scale;
        }

        if (tp->state == TCP_SYN_SENT || tp->ts_ok) {
            uint32_t v = htonl(tcp_ts_now()), e = htonl(tp->ts_recent);

            opts[olen++] = 1; opts[olen++] = 1; opts[olen++] = 8; opts[olen++] = 10;
            memcpy(opts + olen, &v, 4); olen += 4;
            memcpy(opts + olen, &e, 4); olen += 4;
        }

        win = min32(tp->rcvbuf, 65535);
    } else if (rst) {
        win = 0;
    } else {
        if (tp->ts_ok) {
            uint32_t v = htonl(tcp_ts_now()), e = htonl(tp->ts_recent);

            opts[olen++] = 1; opts[olen++] = 1; opts[olen++] = 8; opts[olen++] = 10;
            memcpy(opts + olen, &v, 4); olen += 4;
            memcpy(opts + olen, &e, 4); olen += 4;
        }

        win = tcp_window(tp);
        tp->dup_ack = false;
    }

    struct nbuf *n = nbuf_alloc();
    if (!n) return -ENOMEM;

    if (len) memcpy(nbuf_put(n, len), data, len);

    struct tcp_hdr *h = nbuf_push(n, TCP_HLEN + olen);
    if (!h) {
        nbuf_free(n);
        return -ENOBUFS;
    }

    h->sport = htons(tp->lport);
    h->dport = htons(tp->rport);
    h->seq   = htonl(seq);
    h->ack   = (flags & TCP_ACK) ? htonl(tp->rcv_nxt) : 0;
    h->off   = ((TCP_HLEN + olen) / 4) << 4;
    h->flags = flags;
    h->win   = htons(win);
    h->csum  = 0;
    h->urg   = 0;

    memcpy(h + 1, opts, olen);

    uint32_t total = TCP_HLEN + olen + len;

    h->csum = net_csum(h, total, net_pseudo_sum(tp->laddr, tp->raddr, IPPROTO_TCP, total));

    if (flags & TCP_ACK) {
        tp->ack_now  = false;
        tp->unacked  = 0;
        tp->delack_at = 0;
    }

    return ip_tx(tp->laddr, tp->raddr, IPPROTO_TCP, n);
}

/* the sequence number of what carries no data is the highest sent, not snd_nxt: after a timeout snd_nxt goes back to the oldest
 * segment, and an ACK from there is behind what the peer has received -- outside its window, so all of it is thrown away */
void tcp_send_ack(struct tcb *tp) {
    xmit(tp, tp->snd_max, TCP_ACK, NULL, 0);
}

void tcp_send_probe(struct tcb *tp) {
    xmit(tp, tp->snd_una - 1, TCP_ACK, NULL, 0);
}

void tcp_send_syn(struct tcb *tp) {
    xmit(tp, tp->iss, tp->state == TCP_SYN_RECV ? TCP_SYN | TCP_ACK : TCP_SYN, NULL, 0);
}

void tcp_send_rst(struct tcb *tp) {
    xmit(tp, tp->snd_max, TCP_RST | TCP_ACK, NULL, 0);
}

/* a segment that is for no connection: the answer is a reset that its sender will take (RFC 9293, 3.10.7.1) */
void tcp_reply_rst(uint32_t src, uint32_t dst, const struct tcp_hdr *h, uint32_t seg_len) {
    struct nbuf *n = nbuf_alloc();
    if (!n) return;

    struct tcp_hdr *r = nbuf_put(n, TCP_HLEN);

    r->sport = h->dport;
    r->dport = h->sport;
    r->off   = (TCP_HLEN / 4) << 4;
    r->win   = 0;
    r->csum  = 0;
    r->urg   = 0;

    if (h->flags & TCP_ACK) {
        r->seq   = h->ack;
        r->ack   = 0;
        r->flags = TCP_RST;
    } else {
        r->seq   = 0;
        r->ack   = htonl(ntohl(h->seq) + seg_len);
        r->flags = TCP_RST | TCP_ACK;
    }

    r->csum = net_csum(r, TCP_HLEN, net_pseudo_sum(dst, src, IPPROTO_TCP, TCP_HLEN));

    ip_tx(dst, src, IPPROTO_TCP, n);
}

/* ---- the send queue ---- */

void tcp_fix_cur(struct tcb *tp) {
    if (SEQ_LT(tp->snd_nxt, tp->snd_una)) tp->snd_nxt = tp->snd_una;

    tp->snd_cur = NULL;

    for (list_node *l = tp->sndq.next; l != &tp->sndq; l = l->next) {
        struct tseg *s = container_of(l, struct tseg, node);

        if (SEQ_GE(s->seq, tp->snd_nxt)) {
            tp->snd_cur = s;
            break;
        }
    }
}

/* data in: appended to the segment at the end if that is not sent yet and has room, else in new ones */
uint32_t tcp_queue(struct tcb *tp, const uint8_t *data, uint32_t len) {
    uint32_t room = tp->sndbuf > (int)tp->snd_bytes ? tp->sndbuf - tp->snd_bytes : 0;
    uint32_t done = 0;

    if (len > room) len = room;

    while (done < len) {
        struct tseg *t = NULL;

        if (!list_empty(&tp->sndq)) {
            struct tseg *tail = container_of(tp->sndq.prev, struct tseg, node);

            if (SEQ_GE(tail->seq, tp->snd_nxt) && tail->off == 0 && tail->len < tp->mss) t = tail;
        }

        if (!t) {
            t = kmalloc(sizeof(*t));
            if (!t) break;

            t->seq = tp->snd_end;
            t->len = t->off = 0;

            list_add_tail(&t->node, &tp->sndq);

            if (!tp->snd_cur) tp->snd_cur = t;
        }

        uint32_t take = min32(len - done, tp->mss - t->len);

        memcpy(t->data + t->len, data + done, take);

        t->len        += take;
        done          += take;
        tp->snd_end   += take;
        tp->snd_bytes += take;
    }

    return done;
}

/* seg cut in two, the first n bytes and the rest. false: no memory */
static bool tseg_split(struct tcb *tp, struct tseg *seg, uint32_t n) {
    struct tseg *rest = kmalloc(sizeof(*rest));
    if (!rest) return false;

    rest->seq = seg->seq + n;
    rest->len = seg->len - n;
    rest->off = 0;

    memcpy(rest->data, seg->data + seg->off + n, rest->len);

    seg->len = n;

    list_add(&rest->node, &seg->node);
    (void)tp;
    return true;
}

static void send_data(struct tcb *tp, struct tseg *seg, bool last) {
    xmit(tp, seg->seq, TCP_ACK | (last ? TCP_PSH : 0), seg->data + seg->off, seg->len);
}

/* what may go, goes: segments while the window of the peer and ours (cwnd) allow, then the FIN, and an ACK if it is due */
void tcp_output(struct tcb *tp) {
    bool sent = false;

    switch (tp->state) {
    case TCP_ESTABLISHED:
    case TCP_CLOSE_WAIT:
    case TCP_FIN_WAIT1:
    case TCP_CLOSING:
    case TCP_LAST_ACK:
        break;
    default:
        goto done;
    }

    for (;;) {
        uint32_t flight = tp->snd_nxt - tp->snd_una;
        uint32_t wnd = min32(tp->snd_wnd, tp->cwnd);
        uint32_t avail = wnd > flight ? wnd - flight : 0;
        struct tseg *seg = tp->snd_cur;

        if (seg) {
            if (seg->len > avail) {
                /* a window smaller than a segment, with nothing in flight to open it: the segment is cut to fit */
                if (flight == 0 && tp->snd_wnd > 0 && avail > 0) {
                    if (!tseg_split(tp, seg, avail)) break;
                } else {
                    break;
                }
            }

            bool last = seg->node.next == &tp->sndq;

            /* Nagle: a short segment waits while there is data that is not acknowledged (unless it is the last before a FIN) */
            if (!tp->nodelay && seg->len < tp->mss && flight > 0 && !(last && tp->fin_pending)) break;

            send_data(tp, seg, last);

            if (!tp->rtt_tsc && !tp->ts_ok && SEQ_GE(seg->seq, tp->snd_max)) {
                tp->rtt_seq = seg->seq + seg->len;
                tp->rtt_tsc = rdtsc();
            }

            tp->snd_nxt += seg->len;

            if (SEQ_GT(tp->snd_nxt, tp->snd_max)) tp->snd_max = tp->snd_nxt;

            tp->snd_cur = last ? NULL : container_of(seg->node.next, struct tseg, node);
            sent = true;

            if (!tp->rto_at) tcp_start_rto(tp);
        } else if (tp->fin_pending && !tp->fin_sent) {
            xmit(tp, tp->snd_end, TCP_FIN | TCP_ACK, NULL, 0);

            tp->fin_sent = true;
            tp->snd_nxt  = tp->snd_end + 1;

            if (SEQ_GT(tp->snd_nxt, tp->snd_max)) tp->snd_max = tp->snd_nxt;

            if (tp->state == TCP_ESTABLISHED) tp->state = TCP_FIN_WAIT1;
            else if (tp->state == TCP_CLOSE_WAIT) tp->state = TCP_LAST_ACK;

            sent = true;

            if (!tp->rto_at) tcp_start_rto(tp);
            break;
        } else {
            break;
        }
    }

    /* data to send and a window of nothing, with nothing in flight to ask for the next: ask by the timer */
    if (tp->snd_wnd == 0 && tp->snd_cur && tp->snd_nxt == tp->snd_una) {
        if (!tp->persist_at) {
            tp->persist_backoff = 0;
            tp->persist_at = rdtsc() + ms_to_tsc(tp->rto_ms);
            tcp_arm(tp->persist_at);
        }
    } else if (tp->snd_wnd > 0) {
        tp->persist_at = 0;
    }

done:
    if (!sent && tp->ack_now) tcp_send_ack(tp);
}

/* the reader took data out: if the window that is open now is much more than the one the peer knows, the peer is told */
void tcp_rcv_consumed(struct tcb *tp) {
    if (tp->state == TCP_CLOSED || tp->state == TCP_LISTEN || tp->state == TCP_SYN_SENT || tp->state == TCP_SYN_RECV) return;

    int free = tp->rcvbuf - (int)tp->rx_bytes - (int)tp->ooo_bytes;

    if (free < 0) free = 0;

    uint32_t edge = tp->rcv_nxt + free;

    if (SEQ_GT(edge, tp->rcv_adv) && edge - tp->rcv_adv >= min32(tp->mss, tp->rcvbuf / 2)) {
        tp->ack_now = true;
        tcp_output(tp);
    }
}

/* the oldest segment again, where it is: after three duplicate ACKs, and for each partial one in a recovery */
void tcp_retransmit_head(struct tcb *tp) {
    if (!list_empty(&tp->sndq)) {
        struct tseg *seg = list_first(&tp->sndq, struct tseg, node);

        send_data(tp, seg, seg->node.next == &tp->sndq);
    } else if (tp->fin_sent && tp->snd_una == tp->snd_end) {
        xmit(tp, tp->snd_end, TCP_FIN | TCP_ACK, NULL, 0);
    }
}

/* RFC 6298. srtt is kept eight times, rttvar four times what they are (ms) */
void tcp_rtt_sample(struct tcb *tp, uint32_t ms) {
    int32_t r = ms ? ms : 1;

    if (!tp->rtt_seen) {
        tp->srtt     = r << 3;
        tp->rttvar   = r << 1;
        tp->rtt_seen = true;
    } else {
        int32_t err = r - (int32_t)(tp->srtt >> 3);

        tp->srtt += err;

        if (err < 0) err = -err;

        err -= (int32_t)(tp->rttvar >> 2);
        tp->rttvar += err;
    }

    uint32_t rto = (tp->srtt >> 3) + max32(1, tp->rttvar);

    if (rto < TCP_RTO_MIN_MS) rto = TCP_RTO_MIN_MS;
    if (rto > TCP_RTO_MAX_MS) rto = TCP_RTO_MAX_MS;

    tp->rto_ms = rto;
}

/* the timer of the oldest unacknowledged segment ran out */
bool tcp_rto(struct tcb *tp) {
    switch (tp->state) {
    case TCP_SYN_SENT:
    case TCP_SYN_RECV:
        if (++tp->retries > TCP_SYN_RETRIES) {
            tcp_set_closed(tp, -ETIMEDOUT);
            return false;
        }

        tp->rto_ms = min32(tp->rto_ms * 2, TCP_RTO_MAX_MS);
        tcp_send_syn(tp);
        tcp_start_rto(tp);
        return true;

    case TCP_CLOSED:
    case TCP_LISTEN:
    case TCP_TIME_WAIT:
        return true;

    default:
        break;
    }

    if (tp->snd_una == tp->snd_max) return true;

    if (++tp->retries > TCP_RETRIES) {
        tcp_set_closed(tp, -ETIMEDOUT);
        return false;
    }

    /* the pipe is taken to be empty: slow start from one segment, and what was sent is sent again from the oldest */
    uint32_t flight = tp->snd_max - tp->snd_una;

    tp->ssthresh = max32(flight / 2, 2 * tp->mss);
    tp->cwnd     = tp->mss;
    tp->recovery = false;
    tp->dupacks  = 0;
    tp->rtt_tsc  = 0;
    tp->rto_ms   = min32(tp->rto_ms * 2, TCP_RTO_MAX_MS);

    tp->recover  = tp->snd_max;
    tp->snd_nxt  = tp->snd_una;
    tp->fin_sent = false;
    tcp_fix_cur(tp);

    tcp_start_rto(tp);
    tcp_output(tp);
    return true;
}
