#pragma once

#include <serika/net.h>

/* TCP (RFC 9293 and what it points to: 5681 and 6582 congestion control and NewReno, 6298 the timer, 7323 the window scale
 * and timestamps). all of it is under net_lock. see docs/net.md */

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10
#define TCP_URG 0x20

struct tcp_hdr {
    uint16_t sport, dport;
    uint32_t seq, ack;
    uint8_t  off;             /* the length of the header in dwords, in the top four bits */
    uint8_t  flags;
    uint16_t win;
    uint16_t csum;
    uint16_t urg;
} __attribute__((packed));

#define TCP_HLEN 20

enum {
    TCP_CLOSED, TCP_LISTEN, TCP_SYN_SENT, TCP_SYN_RECV, TCP_ESTABLISHED, TCP_FIN_WAIT1, TCP_FIN_WAIT2,
    TCP_CLOSE_WAIT, TCP_CLOSING, TCP_LAST_ACK, TCP_TIME_WAIT,
};

/* sequence numbers wrap */
#define SEQ_LT(a, b)  ((int32_t)((a) - (b)) < 0)
#define SEQ_LE(a, b)  ((int32_t)((a) - (b)) <= 0)
#define SEQ_GT(a, b)  ((int32_t)((a) - (b)) > 0)
#define SEQ_GE(a, b)  ((int32_t)((a) - (b)) >= 0)

#define TCP_SEG_CAP     1460      /* the data of a segment: MTU 1500 less the headers */
#define TCP_DEFAULT_MSS 536
#define TCP_INIT_CWND   10        /* segments */
#define TCP_RTO_INIT_MS 1000
#define TCP_RTO_MIN_MS  200
#define TCP_RTO_MAX_MS  60000
#define TCP_DELACK_MS   40
#define TCP_MSL_S       30        /* TIME_WAIT is twice it */
#define TCP_SYN_RETRIES 6
#define TCP_RETRIES     15
#define TCP_SYN_QUEUE   128       /* half open connections of a listener, at most */

/* data waiting to be sent or acknowledged: one is a part of a stream, the bytes from seq. it is never cut in the middle
 * but when the peer's window is smaller than it, and trimmed in front when acknowledged in part */
struct tseg {
    list_node node;
    uint32_t  seq;
    uint16_t  len, off;       /* data[off .. off + len) */
    uint8_t   data[TCP_SEG_CAP];
};

struct tcb {
    list_node      node;      /* tcp_conns (every connection but the listeners) or tcp_listen */
    bool           listed;
    int            state;
    struct isock  *sk;        /* NULL: closed by its owner (or not accepted yet), the connection lives on by itself */
    int            err;       /* -errno, taken once by the socket */

    uint32_t       laddr, raddr;     /* host order */
    uint16_t       lport, rport;

    /* a listener, and a connection that is still on its queues */
    struct tcb    *parent;
    list_node      accept_node;
    bool           in_accept;
    list_node      accept_q;
    unsigned       nr_accept, nr_syn, backlog;

    /* send: snd_una .. snd_nxt is on its way, snd_nxt .. snd_end is queued and not sent */
    uint32_t       iss, snd_una, snd_nxt, snd_max, snd_end, snd_wl1, snd_wl2;
    uint32_t       snd_wnd;
    list_node      sndq;
    struct tseg   *snd_cur;   /* the first segment that is not sent (again); NULL: none */
    uint32_t       snd_bytes; /* the data in sndq */
    bool           fin_pending, fin_sent;
    uint16_t       mss;       /* of the data in a segment */
    uint8_t        snd_scale, rcv_scale;
    bool           wscale_offered, ts_ok;
    bool           nodelay;

    /* congestion */
    uint32_t       cwnd, ssthresh, dupacks, recover;
    bool           recovery;
    uint32_t       srtt, rttvar;      /* ms, srtt * 8 and rttvar * 4 */
    uint32_t       rto_ms;
    bool           rtt_seen;
    uint32_t       rtt_seq;           /* the segment timed when there are no timestamps */
    uint64_t       rtt_tsc;
    uint32_t       retries;

    /* timers: tsc, 0 for none */
    uint64_t       rto_at, persist_at, delack_at, tw_at, ka_at;
    uint32_t       persist_backoff;

    /* receive */
    uint32_t       irs, rcv_nxt, rcv_adv;
    list_node      rxq;       /* nbufs of data in order */
    uint32_t       rx_bytes;
    list_node      ooo;       /* nbufs of data after a gap, by seq (in nbuf.addr) */
    uint32_t       ooo_bytes;
    bool           fin_rcvd, rx_shut;
    bool           ack_now;
    bool           dup_ack;   /* the ACK that goes is for a segment out of order: its window must be the same as the last (the sender counts duplicates) */
    uint32_t       unacked;   /* segments received since the last ACK went */
    uint32_t       ts_recent;
    int            rcvbuf, sndbuf;

    bool           keepalive;
    uint32_t       ka_idle, ka_intvl, ka_cnt, ka_probes;
    uint64_t       last_rx;
};

/* tcp.c: connections, timers */
extern list_node tcp_conns, tcp_listen;

void        tcp_init(void);
struct tcb *tcb_new(void);
void        tcb_free(struct tcb *tp);
struct tcb *tcp_lookup(uint32_t laddr, uint16_t lport, uint32_t raddr, uint16_t rport);
struct tcb *tcp_find_listener(uint32_t laddr, uint16_t lport);
void        tcp_set_closed(struct tcb *tp, int err); /* tp may be gone (no owner) when it returns */
void        tcp_arm(uint64_t when);
void        tcp_keepalive_start(struct tcb *tp);
void        tcp_close(struct tcb *tp);               /* its owner is done with it. tp is not the owner's after */
void        tcp_tick(void);
void        tcp_dump(void);                          /* every connection to the console: for a test that does not end */
uint32_t    tcp_isn(uint32_t laddr, uint16_t lport, uint32_t raddr, uint16_t rport);
uint32_t    tcp_ts_now(void);
uint8_t     tcp_scale_for(int rcvbuf);
bool        tcp_port_used(uint16_t port);             /* by a connection or a listener, for the ports that are picked */

/* tcp_out.c */
void tcp_output(struct tcb *tp);                      /* what may go, goes. then an ACK, if one is due */
void tcp_send_ack(struct tcb *tp);
void tcp_send_probe(struct tcb *tp);                  /* an ACK with a sequence number that is old: the peer answers it with its window */
void tcp_send_syn(struct tcb *tp);                    /* SYN, or SYN+ACK in SYN_RECV */
void tcp_send_rst(struct tcb *tp);
void tcp_reply_rst(uint32_t src, uint32_t dst, const struct tcp_hdr *h, uint32_t seg_len); /* for a segment that fits no connection */
void tcp_retransmit_head(struct tcb *tp);
bool tcp_rto(struct tcb *tp);                        /* false: the connection was ended (and tp may be gone) */
void tcp_fix_cur(struct tcb *tp);
uint32_t tcp_queue(struct tcb *tp, const uint8_t *data, uint32_t len); /* how many were queued (the room is limited) */
uint32_t tcp_window(struct tcb *tp);                  /* the window field for the next segment, and rcv_adv moved with it */
void tcp_rtt_sample(struct tcb *tp, uint32_t ms);
void tcp_start_rto(struct tcb *tp);
void tcp_rcv_consumed(struct tcb *tp);               /* the reader took data: the window may be worth telling */

/* tcp_in.c */
void tcp_rx(struct netdev *dev, const struct ip_hdr *iph, struct nbuf *n);
void tcp_icmp_err(uint32_t laddr, uint16_t lport, uint32_t raddr, uint16_t rport, int err);

/* tcp_sock.c: the calls of a stream socket */
long     tcp_op_bind(struct file *f, long uaddr, long ulen);
long     tcp_op_connect(struct file *f, long uaddr, long ulen);
long     tcp_op_listen(struct file *f, long backlog);
long     tcp_op_accept(struct file *f, long uaddr, long ulenp, long flags);
long     tcp_op_getname(struct file *f, long uaddr, long ulenp, bool peer);
long     tcp_op_shutdown(struct file *f, long how);
long     tcp_op_sendmsg(struct file *f, struct msghdr *mh, const struct iovec *iov, unsigned flags);
long     tcp_op_recvmsg(struct file *f, struct msghdr *mh, const struct iovec *iov, unsigned flags);
long     tcp_op_setsockopt(struct file *f, long level, long name, long uval, long len);
long     tcp_op_getsockopt(struct file *f, long level, long name, long uval, long ulenp);
long     tcp_op_read(struct file *f, void *buf, size_t n);
long     tcp_op_write(struct file *f, const void *buf, size_t n);
uint32_t tcp_op_poll(struct inode *inode, struct poll_table *pt);
void     tcp_op_release(struct inode *inode);
void     tcp_sock_release(struct isock *sk);

#define SOL_TCP 6
