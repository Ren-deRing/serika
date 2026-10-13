#pragma once

#include <serika/list.h>
#include <serika/lock.h>
#include <serika/wait.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* linux's values: this is the abi */
#define AF_UNIX  1
#define AF_INET  2
#define AF_NETLINK 16
#define NETLINK_KOBJECT_UEVENT 15
#define AF_LOCAL 1

#define SOCK_STREAM    1
#define SOCK_DGRAM     2
#define SOCK_RAW       3
#define SOCK_SEQPACKET 5
#define SOCK_TYPE_MASK 0xf
#define SOCK_NONBLOCK  04000
#define SOCK_CLOEXEC   02000000

#define SOL_SOCKET 1

#define SO_DEBUG      1
#define SO_REUSEADDR  2
#define SO_TYPE       3
#define SO_ERROR      4
#define SO_DONTROUTE  5
#define SO_BROADCAST  6
#define SO_SNDBUF     7
#define SO_RCVBUF     8
#define SO_KEEPALIVE  9
#define SO_OOBINLINE  10
#define SO_NO_CHECK   11
#define SO_PRIORITY   12
#define SO_LINGER     13
#define SO_REUSEPORT  15
#define SO_PASSCRED   16
#define SO_PEERCRED   17
#define SO_RCVLOWAT   18
#define SO_SNDLOWAT   19
#define SO_RCVTIMEO   20
#define SO_SNDTIMEO   21
#define SO_ACCEPTCONN 30
#define SO_PEERSEC    31
#define SO_SNDBUFFORCE 32
#define SO_RCVBUFFORCE 33
#define SO_PASSSEC    34
#define SO_ATTACH_FILTER 26
#define SO_ATTACH_BPF    50
#define SO_PROTOCOL   38
#define SO_DOMAIN     39

#define MSG_OOB          0x1
#define MSG_PEEK         0x2
#define MSG_DONTROUTE    0x4
#define MSG_CTRUNC       0x8
#define MSG_TRUNC        0x20
#define MSG_DONTWAIT     0x40
#define MSG_EOR          0x80
#define MSG_WAITALL      0x100
#define MSG_NOSIGNAL     0x4000
#define MSG_CMSG_CLOEXEC 0x40000000

#define SCM_RIGHTS      1
#define SCM_CREDENTIALS 2
#define SCM_MAX_FD      253

#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

#define SOMAXCONN 4096

#define UNIX_PATH_MAX 108

struct sockaddr_un {
    uint16_t sun_family;
    char     sun_path[UNIX_PATH_MAX];
};

struct ucred {
    int32_t  pid;
    uint32_t uid;
    uint32_t gid;
};

struct msghdr {
    uint64_t msg_name;
    uint32_t msg_namelen;
    uint32_t __pad0;
    uint64_t msg_iov;
    uint64_t msg_iovlen;
    uint64_t msg_control;
    uint64_t msg_controllen;
    int32_t  msg_flags;
    int32_t  __pad1;
};

struct mmsghdr {
    struct msghdr msg_hdr;
    uint32_t      msg_len;
    uint32_t      __pad;
};

struct cmsghdr {
    uint64_t cmsg_len;
    int32_t  cmsg_level;
    int32_t  cmsg_type;
};

#define CMSG_ALIGN(len)  (((len) + 7) & ~7ULL)
#define CMSG_LEN(len)    (sizeof(struct cmsghdr) + (len))
#define CMSG_SPACE(len)  (sizeof(struct cmsghdr) + CMSG_ALIGN(len))

/* ---- inside the kernel ---- */

/* a name: a path as given (the file is the inode's), or an abstract one, which starts with a nul. len counts the
 * bytes of sun_path used */
struct unix_addr {
    uint16_t len;
    char     name[UNIX_PATH_MAX];
};

/* a message waiting to be read */
struct skb {
    list_node         node;
    uint8_t          *data;
    size_t            len;
    size_t            off;       /* stream: how much was read */
    int               order;     /* -1: kmalloc, else a block of pages */
    struct file     **fds;       /* passed with it, our references */
    unsigned          nfds;
    struct ucred      cred;
    struct unix_addr  from;      /* a datagram's sender */
    bool              has_from;
};

/* every kind of socket starts with this, and the inode of a socket points to it: what the calls on it do is the kind's. the
 * kinds are unix (below) and inet (net/inet.h). the calls are those of socket(2); f is the file of the socket, a user
 * address or buffer is in user memory, and the return is a value or -errno */
struct file;
struct inode;
struct iovec;
struct poll_table;
struct sock;

struct proto_ops {
    int      (*bind)(struct file *f, long uaddr, long ulen);
    int      (*connect)(struct file *f, long uaddr, long ulen);
    int      (*listen)(struct file *f, long backlog);
    long     (*accept)(struct file *f, long uaddr, long ulenp, long flags);   /* the new file's descriptor */
    long     (*getname)(struct file *f, long uaddr, long ulenp, bool peer);
    int      (*shutdown)(struct file *f, long how);
    long     (*sendmsg)(struct file *f, struct msghdr *mh, const struct iovec *iov, unsigned flags);
    long     (*recvmsg)(struct file *f, struct msghdr *mh, const struct iovec *iov, unsigned flags);
    long     (*setsockopt)(struct file *f, long level, long name, long uval, long len);
    long     (*getsockopt)(struct file *f, long level, long name, long uval, long ulenp);

    /* the file's own: read(2), write(2), poll, ioctl */
    long     (*read)(struct file *f, void *buf, size_t n);
    long     (*write)(struct file *f, const void *buf, size_t n);
    uint32_t (*poll)(struct inode *inode, struct poll_table *pt);
    long     (*ioctl)(struct inode *inode, unsigned long cmd, unsigned long arg);

    void     (*release)(struct inode *inode); /* the last file of it is closed (the inode may live on) */
    void     (*put)(struct sock *sk);         /* the inode's reference to it goes */
};

struct sock {
    const struct proto_ops *ops;
};

extern const struct proto_ops unix_proto_ops;

/* a unix socket. sk->lock covers all of it except what is marked, and no code holds two of them */
struct usock {
    struct sock      sock;
    uint32_t         refs;       /* atomic: the inode, a peer, the accept queue, whoever is using it */
    spinlock_t       lock;
    int              type;

    bool             dead;       /* its last file closed */
    bool             listening;
    bool             netlink;    /* an AF_NETLINK uevent socket, which nothing is ever sent on: libudev's monitor */
    bool             has_addr;
    bool             abstract;
    struct unix_addr addr;
    list_node        abstract_node; /* abstract_lock */

    struct usock    *peer;       /* connected: our reference */
    bool             peer_closed;
    bool             rx_eof;     /* nothing more will come in */
    bool             rx_shut;    /* shutdown(SHUT_RD): a sender is refused */
    bool             tx_shut;    /* shutdown(SHUT_WR), or the peer is gone: sending is EPIPE */

    list_node        rx;         /* skbs */
    size_t           rx_bytes;

    list_node        accept_q;   /* connected sockets not accepted yet, our references */
    unsigned         nr_accept;
    unsigned         backlog;
    list_node        accept_node;

    struct ucred     cred;       /* of who made it, and who connected or listened */
    struct ucred     peercred;

    bool             passcred;
    int              sndbuf, rcvbuf;
    uint64_t         rcvtimeo, sndtimeo; /* tsc ticks. 0: none */
    uint32_t         flags;      /* the options that only have to be given back: SO_REUSEADDR, SO_KEEPALIVE ... */
};

/* the kernel's side of one send and one receive. the buffers are the caller's, in the kernel */
struct unix_msg {
    const void       *data;
    size_t            len;
    struct file      *fds[SCM_MAX_FD];
    unsigned          nfds;
    bool              has_cred;
    struct ucred      cred;
    struct usock     *to;        /* a datagram's target. NULL: the connected peer */
};

struct unix_rx {
    void             *buf;
    size_t            cap;
    size_t            len;       /* copied into buf */
    size_t            full;      /* the message's own size: a datagram cut short has more than len */
    struct file      *fds[SCM_MAX_FD];
    unsigned          nfds;
    bool              has_cred;
    struct ucred      cred;
    bool              has_from;
    struct unix_addr  from;
    bool              trunc;
};

struct poll_table;

void net_init(void); /* before the first socket */

struct usock *unix_create(int type);
void unix_get(struct usock *sk);
void unix_put(struct usock *sk);

int  unix_pair(struct usock *a, struct usock *b);
int  unix_bind(struct usock *sk, const struct unix_addr *addr, bool abstract, bool autobind);
struct usock *unix_lookup_abstract(const struct unix_addr *addr);
int  unix_listen(struct usock *sk, int backlog);
int  unix_connect(struct usock *sk, struct usock *target, bool nonblock);
struct usock *unix_accept(struct usock *sk, bool nonblock, int *err);
long unix_send(struct usock *sk, struct unix_msg *m, bool nonblock);
long unix_recv(struct usock *sk, struct unix_rx *rx, unsigned flags, bool nonblock);
int  unix_shutdown(struct usock *sk, int how);
void unix_release(struct usock *sk);
uint32_t unix_poll(struct usock *sk, struct poll_table *pt);
long unix_inq(struct usock *sk);
long unix_outq(struct usock *sk);
void unix_msg_drop(struct unix_msg *m); /* the references of a message that was not sent */
void unix_rx_drop(struct unix_rx *rx);
