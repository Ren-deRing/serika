#include <serika/cpu.h>
#include <serika/errno.h>
#include <serika/net.h>
#include <serika/printk.h>
#include <serika/task.h>
#include <serika/random.h>
#include <serika/slab.h>
#include <serika/string.h>
#include <serika/tcp.h>
#include <serika/timer.h>

#include <asm/arch.h>

/* TCP, the connections: the tables, how a connection is made, ends, and the timers. the way segments come in is
 * tcp_in.c, how they go out tcp_out.c, and the calls of a socket are tcp_sock.c */

list_node tcp_conns = LIST_HEAD_INIT(tcp_conns);
list_node tcp_listen = LIST_HEAD_INIT(tcp_listen);

static struct timer tcp_timer;
static uint8_t      secret[16];

void tcp_init(void) {
    get_random_bytes(secret, sizeof(secret));
}

/* ---- numbers ---- */

uint32_t tcp_ts_now(void) {
    return rdtsc() / (this_cpu_read(tsc_freq_hz) / 1000);
}

/* RFC 6528: a hash of the connection and a secret, and a clock of 4 us */
uint32_t tcp_isn(uint32_t laddr, uint16_t lport, uint32_t raddr, uint16_t rport) {
    uint32_t h = 2166136261U;
    uint8_t in[12 + sizeof(secret)];

    memcpy(in, &laddr, 4);
    memcpy(in + 4, &raddr, 4);
    memcpy(in + 8, &lport, 2);
    memcpy(in + 10, &rport, 2);
    memcpy(in + 12, secret, sizeof(secret));

    for (size_t i = 0; i < sizeof(in); i++) h = (h ^ in[i]) * 16777619U;

    return h + (uint32_t)(rdtsc() / (this_cpu_read(tsc_freq_hz) / 250000));
}

/* the shift that lets a window of rcvbuf bytes be said in 16 bits */
uint8_t tcp_scale_for(int rcvbuf) {
    uint8_t s = 0;

    while (s < 14 && (65535U << s) < (uint32_t)rcvbuf) s++;

    return s;
}

/* ---- connections ---- */

struct tcb *tcb_new(void) {
    struct tcb *tp = kmalloc(sizeof(*tp));
    if (!tp) return NULL;

    memset(tp, 0, sizeof(*tp));

    list_init(&tp->accept_q);
    list_init(&tp->sndq);
    list_init(&tp->rxq);
    list_init(&tp->ooo);

    tp->state    = TCP_CLOSED;
    tp->mss      = TCP_DEFAULT_MSS;
    tp->rcvbuf   = tp->sndbuf = UDP_RCVBUF;
    tp->rto_ms   = TCP_RTO_INIT_MS;
    tp->ssthresh = 0x7fffffff;
    tp->ka_idle  = 7200;
    tp->ka_intvl = 75;
    tp->ka_cnt   = 9;
    return tp;
}

static void purge_send(struct tcb *tp) {
    while (!list_empty(&tp->sndq)) {
        struct tseg *s = list_first(&tp->sndq, struct tseg, node);

        list_del(&s->node);
        kfree(s);
    }

    tp->snd_cur   = NULL;
    tp->snd_bytes = 0;
}

static void purge_list(list_node *head) {
    while (!list_empty(head)) {
        struct nbuf *n = list_first(head, struct nbuf, node);

        list_del(&n->node);
        nbuf_free(n);
    }
}

void tcb_free(struct tcb *tp) {
    if (tp->listed) list_del(&tp->node);

    purge_send(tp);
    purge_list(&tp->rxq);
    purge_list(&tp->ooo);
    kfree(tp);
}

struct tcb *tcp_lookup(uint32_t laddr, uint16_t lport, uint32_t raddr, uint16_t rport) {
    for (list_node *l = tcp_conns.next; l != &tcp_conns; l = l->next) {
        struct tcb *tp = container_of(l, struct tcb, node);

        if (tp->lport == lport && tp->rport == rport && tp->raddr == raddr && tp->laddr == laddr) return tp;
    }

    return NULL;
}

struct tcb *tcp_find_listener(uint32_t laddr, uint16_t lport) {
    struct tcb *any = NULL;

    for (list_node *l = tcp_listen.next; l != &tcp_listen; l = l->next) {
        struct tcb *tp = container_of(l, struct tcb, node);

        if (tp->lport != lport) continue;
        if (tp->laddr == laddr) return tp;
        if (tp->laddr == 0) any = tp;
    }

    return any;
}

bool tcp_port_used(uint16_t port) {
    for (list_node *l = tcp_conns.next; l != &tcp_conns; l = l->next)
        if (container_of(l, struct tcb, node)->lport == port) return true;

    for (list_node *l = tcp_listen.next; l != &tcp_listen; l = l->next)
        if (container_of(l, struct tcb, node)->lport == port) return true;

    return false;
}

/* the connection is over: the owner sees err (once) and the end. a connection that is not owned goes. what is waiting to be
 * read stays for the owner */
void tcp_set_closed(struct tcb *tp, int err) {
    tp->state = TCP_CLOSED;

    if (err && !tp->err) tp->err = err;

    tp->rto_at = tp->persist_at = tp->delack_at = tp->tw_at = tp->ka_at = 0;
    tp->ack_now = false;

    purge_send(tp);
    purge_list(&tp->ooo);
    tp->ooo_bytes = 0;

    if (tp->listed) {
        list_del(&tp->node);
        tp->listed = false;
    }

    if (tp->parent) {
        struct tcb *p = tp->parent;

        if (tp->in_accept) {
            list_del(&tp->accept_node);
            p->nr_accept--;
            tp->in_accept = false;
        } else if (p->nr_syn) {
            p->nr_syn--;
        }

        tp->parent = NULL;
    }

    wake_up(&net_wq);

    if (!tp->sk) tcb_free(tp);
}

/* the owner is done: what is sent is sent, and the end goes on by itself (an orphan), or it is over now */
void tcp_close(struct tcb *tp) {
    tp->sk = NULL;

    /* nobody will read it */
    while (!list_empty(&tp->rxq)) {
        struct nbuf *n = list_first(&tp->rxq, struct nbuf, node);

        list_del(&n->node);
        nbuf_free(n);
    }

    bool unread = tp->rx_bytes > 0;

    tp->rx_bytes = 0;

    switch (tp->state) {
    case TCP_CLOSED:
        tcb_free(tp);
        return;

    case TCP_LISTEN: {
        /* what is waiting to be accepted, and the half open ones, are given up */
        list_node *l = tcp_conns.next;

        while (l != &tcp_conns) {
            struct tcb *c = container_of(l, struct tcb, node);

            l = l->next;

            if (c->parent != tp) continue;

            if (c->state != TCP_SYN_RECV) tcp_send_rst(c);

            tcp_set_closed(c, 0);
        }

        tcp_set_closed(tp, 0);
        return;
    }

    case TCP_SYN_SENT:
        tcp_set_closed(tp, 0);
        return;

    case TCP_SYN_RECV:
        tcp_send_rst(tp);
        tcp_set_closed(tp, 0);
        return;

    case TCP_ESTABLISHED:
    case TCP_CLOSE_WAIT:
        /* data that came and was not read: a reset, as the peer is told that it was lost */
        if (unread) {
            tcp_send_rst(tp);
            tcp_set_closed(tp, 0);
            return;
        }

        tp->fin_pending = true;
        tcp_output(tp);
        break;

    case TCP_FIN_WAIT2:
        tp->tw_at = rdtsc() + (uint64_t)2 * TCP_MSL_S * this_cpu_read(tsc_freq_hz);
        tcp_arm(tp->tw_at);
        break;

    default:
        break;
    }
}

/* where every connection is. net_lock is only tried: if it is held for good, that is what the line says */
void tcp_dump(void) {
    bool got = mutex_trylock(&net_lock);
    struct task *owner = __atomic_load_n(&net_lock.owner, __ATOMIC_RELAXED);

    printk("tcp: net_lock %s\n", got ? "was free" : "IS HELD");

    if (!got && owner) printk("tcp: owner task %lu\n", owner->id);

    static const char *const names[] = { "CLOSED", "LISTEN", "SYN_SENT", "SYN_RECV", "ESTABLISHED", "FIN_WAIT1", "FIN_WAIT2",
                                         "CLOSE_WAIT", "CLOSING", "LAST_ACK", "TIME_WAIT" };

    for (int pass = 0; pass < 2; pass++) {
        list_node *head = pass ? &tcp_listen : &tcp_conns;

        for (list_node *l = head->next; l != head; l = l->next) {
            struct tcb *tp = container_of(l, struct tcb, node);

            printk("tcp: %u:%u > %u:%u %s sk %d una %u nxt %u max %u end %u wnd %u cwnd %u ssth %u rto %ums retr %u dup %u "
                   "rcv %u adv %u rxq %u ooo %u sndq %u%s%s\n",
                   tp->laddr & 0xff, tp->lport, tp->raddr & 0xff, tp->rport, names[tp->state], tp->sk != NULL,
                   tp->snd_una - tp->iss, tp->snd_nxt - tp->iss, tp->snd_max - tp->iss, tp->snd_end - tp->iss,
                   tp->snd_wnd, tp->cwnd, tp->ssthresh, tp->rto_ms, tp->retries, tp->dupacks,
                   tp->rcv_nxt - tp->irs, tp->rcv_adv - tp->irs, tp->rx_bytes, tp->ooo_bytes, tp->snd_bytes,
                   tp->fin_pending ? " fin_pending" : "", tp->persist_at ? " persist" : "");
        }
    }

    if (got) mutex_unlock(&net_lock);
}

/* ---- timers ---- */

static void tcp_timer_fn(struct timer *t) {
    (void)t;

    netd_kick(NETD_TCP);
}

/* the timer fires no later than `when` */
void tcp_arm(uint64_t when) {
    if (tcp_timer.queued && tcp_timer.expires <= when) return;

    timer_del(&tcp_timer);

    tcp_timer.expires = when;
    tcp_timer.fn      = tcp_timer_fn;
    timer_add(&tcp_timer);
}

static void soonest(uint64_t *next, uint64_t at) {
    if (at && (!*next || at < *next)) *next = at;
}

/* netd: whatever is due, and the timer set for the next */
void tcp_tick(void) {
    uint64_t now = rdtsc(), next = 0;

    list_node *l = tcp_conns.next;

    while (l != &tcp_conns) {
        struct tcb *tp = container_of(l, struct tcb, node);

        l = l->next;                  /* tp may go below */

        if (tp->tw_at && now >= tp->tw_at) {
            tcp_set_closed(tp, 0);
            continue;
        }

        if (tp->rto_at && now >= tp->rto_at) {
            tp->rto_at = 0;

            if (!tcp_rto(tp)) continue;
        }

        if (tp->persist_at && now >= tp->persist_at) {
            /* the window is shut: a segment to ask what it is now (the old sequence number, which the peer answers) */
            tp->persist_backoff++;

            uint64_t wait = (uint64_t)tp->rto_ms << (tp->persist_backoff < 8 ? tp->persist_backoff : 8);

            if (wait > TCP_RTO_MAX_MS) wait = TCP_RTO_MAX_MS;

            tp->persist_at = now + wait * this_cpu_read(tsc_freq_hz) / 1000;

            tcp_send_probe(tp);
        }

        if (tp->delack_at && now >= tp->delack_at) {
            tp->delack_at = 0;
            tp->ack_now = true;
            tcp_output(tp);
        }

        if (tp->ka_at && now >= tp->ka_at) {
            if (tp->ka_probes >= tp->ka_cnt) {
                tcp_set_closed(tp, -ETIMEDOUT);
                continue;
            }

            tp->ka_probes++;
            tp->ka_at = now + (uint64_t)tp->ka_intvl * this_cpu_read(tsc_freq_hz);

            tcp_send_probe(tp);
        }

        soonest(&next, tp->rto_at);
        soonest(&next, tp->persist_at);
        soonest(&next, tp->delack_at);
        soonest(&next, tp->tw_at);
        soonest(&next, tp->ka_at);
    }

    if (next) {
        timer_del(&tcp_timer);

        tcp_timer.expires = next;
        tcp_timer.fn      = tcp_timer_fn;
        timer_add(&tcp_timer);
    }
}
