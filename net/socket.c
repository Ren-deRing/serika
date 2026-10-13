#include <serika/buddy.h>
#include <serika/cpu.h>
#include <serika/errno.h>
#include <serika/fcntl.h>
#include <serika/mm.h>
#include <serika/net.h>
#include <serika/page.h>
#include <serika/poll.h>
#include <serika/signal.h>
#include <serika/slab.h>
#include <serika/socket.h>
#include <serika/stat.h>
#include <serika/string.h>
#include <serika/syscall.h>
#include <serika/task.h>
#include <serika/uio.h>
#include <serika/vfs.h>

#include <asm/arch.h>

/* the system calls of sockets: what is in user memory is copied in before a socket is touched, and copied out after,
 * so that no socket lock is held across a page fault (that is net/unix.c's rule) */

#define UNIX_CHUNK   (64 * 1024)  /* a stream send is cut into pieces this big */
#define UNIX_MSG_MAX (128 * 1024) /* the most a datagram or a seqpacket message holds, and a read takes */
#define CONTROL_MAX  (64 * 1024)

#define FIONREAD 0x541b /* also SIOCINQ */
#define SIOCOUTQ 0x5411

static const struct inode_ops sock_ops;

/* the socket of a file, of whatever kind: NULL if it is not one */
static struct sock *sock_of(struct file *f) {
    return f->inode->ops == &sock_ops ? f->inode->priv : NULL;
}

/* ... and of the unix kind, in the functions that sock_of()'s ops table has brought here */
static struct usock *usock_of(struct file *f) {
    return f->inode->priv;
}

/* ---- buffers: kmalloc stops at a page ---- */

struct kbuf {
    void *p;
    int   order; /* -1: kmalloc */
};

static bool kbuf_alloc(struct kbuf *b, size_t n) {
    if (n <= PAGE_SIZE) {
        b->order = -1;
        b->p = kmalloc(n ? n : 1);
    } else {
        b->order = SIZE_TO_ORDER(n);

        uintptr_t phys = (uintptr_t)alloc_pages(b->order);
        b->p = phys ? (void *)P2V(phys) : NULL;
    }

    return b->p != NULL;
}

static void kbuf_free(struct kbuf *b) {
    if (!b->p) return;

    if (b->order >= 0) free_pages((void *)V2P((uintptr_t)b->p), b->order);
    else               kfree(b->p);

    b->p = NULL;
}

/* ---- the file of a socket ---- */

static long unix_op_read(struct file *f, void *buf, size_t n) {
    struct usock *sk = f->inode->priv;
    struct unix_rx rx = { .buf = buf, .cap = n };

    long r = unix_recv(sk, &rx, 0, f->flags & O_NONBLOCK);

    unix_rx_drop(&rx); /* read() has nowhere to put the files that came with it */
    return r;
}

static long unix_op_write(struct file *f, const void *buf, size_t n) {
    struct usock *sk = f->inode->priv;
    struct unix_msg m = { .data = buf, .len = n };

    long r = unix_send(sk, &m, f->flags & O_NONBLOCK);

    if (r == -EPIPE) send_sigpipe();
    return r;
}

static uint32_t unix_op_poll(struct inode *inode, struct poll_table *pt) {
    return unix_poll(inode->priv, pt);
}

static long unix_op_ioctl(struct inode *inode, unsigned long cmd, unsigned long arg) {
    struct usock *sk = inode->priv;
    int32_t n;

    switch (cmd) {
        case FIONREAD:
            n = unix_inq(sk);
            return copy_to_user(arg, &n, sizeof(n));

        case SIOCOUTQ:
            n = unix_outq(sk);
            return copy_to_user(arg, &n, sizeof(n));
    }

    return -ENOTTY;
}

/* the last file of it is closed. the inode may live on: a path names it */
static void unix_op_release(struct inode *inode) {
    unix_release(inode->priv);
}

static void unix_op_put(struct sock *sk) {
    unix_put((struct usock *)sk);
}

/* what the file of a socket does is up to the kind of socket */
static long sock_read(struct file *f, void *buf, size_t n) {
    struct sock *sk = f->inode->priv;

    return sk->ops->read(f, buf, n);
}

static long sock_write(struct file *f, const void *buf, size_t n) {
    struct sock *sk = f->inode->priv;

    return sk->ops->write(f, buf, n);
}

static uint32_t sock_poll(struct inode *inode, uint32_t fflags, struct poll_table *pt) {
    struct sock *sk = inode->priv;

    (void)fflags;
    return sk->ops->poll(inode, pt);
}

static long sock_ioctl(struct inode *inode, unsigned long cmd, unsigned long arg) {
    struct sock *sk = inode->priv;

    return sk->ops->ioctl(inode, cmd, arg);
}

static void sock_release(struct inode *inode, uint32_t fflags) {
    struct sock *sk = inode->priv;

    (void)fflags;
    sk->ops->release(inode);
}

static void sock_evict(struct inode *inode) {
    struct sock *sk = inode->priv;

    sk->ops->put(sk);
}

static const struct inode_ops sock_ops = {
    .stream_read  = sock_read,
    .stream_write = sock_write,
    .poll         = sock_poll,
    .ioctl        = sock_ioctl,
    .evict        = sock_evict,
    .stream       = true,
    .release      = sock_release,
};

/* a file for the socket, which takes the caller's reference whether it works or not */
static struct file *sock_file_new(struct sock *sk, uint32_t flags) {
    struct inode *inode = inode_alloc(&sock_ops, S_IFSOCK | 0777);
    if (!inode) {
        sk->ops->put(sk);
        return NULL;
    }

    inode->priv = sk; /* the inode's reference now */

    struct file *f = file_alloc(inode, O_RDWR | (flags & O_NONBLOCK));
    if (!f) iput(inode);

    return f;
}

/* ---- socket, socketpair ---- */

static int check_type(long domain, long type, long protocol) {
    /* the uevent socket of libudev's monitor, which hears nothing: there are no devices coming and going */
    if (domain == AF_NETLINK) {
        if (type & ~(SOCK_TYPE_MASK | SOCK_NONBLOCK | SOCK_CLOEXEC)) return -EINVAL;
        if ((type & SOCK_TYPE_MASK) != SOCK_RAW && (type & SOCK_TYPE_MASK) != SOCK_DGRAM) return -ESOCKTNOSUPPORT;

        return protocol == NETLINK_KOBJECT_UEVENT ? SOCK_DGRAM : -EPROTONOSUPPORT;
    }

    if (domain != AF_UNIX) return -EAFNOSUPPORT;
    if (type & ~(SOCK_TYPE_MASK | SOCK_NONBLOCK | SOCK_CLOEXEC)) return -EINVAL;

    int t = type & SOCK_TYPE_MASK;

    if (t != SOCK_STREAM && t != SOCK_DGRAM && t != SOCK_SEQPACKET) return -ESOCKTNOSUPPORT;
    if (protocol != 0) return -EPROTONOSUPPORT;

    return t;
}

long sys_socket(long domain, long type, long protocol) {
    struct sock *sk;

    if (domain == AF_INET) {
        if (type & ~(SOCK_TYPE_MASK | SOCK_NONBLOCK | SOCK_CLOEXEC)) return -EINVAL;

        int err;

        sk = inet_create(type & SOCK_TYPE_MASK, protocol, &err);
        if (!sk) return err;
    } else {
        int t = check_type(domain, type, protocol);
        if (t < 0) return t;

        struct usock *usk = unix_create(t);
        if (!usk) return -ENOMEM;

        usk->netlink = domain == AF_NETLINK;
        sk = &usk->sock;
    }

    struct file *f = sock_file_new(sk, type);
    if (!f) return -ENOMEM;

    int fd = fd_install(current_task()->files, f, type & SOCK_CLOEXEC, 0);
    if (fd < 0) vfs_close(f);

    return fd;
}

long sys_socketpair(long domain, long type, long protocol, long usv) {
    int t = check_type(domain, type, protocol);
    if (t < 0) return t;

    struct usock *a = unix_create(t), *b = unix_create(t);
    if (!a || !b) {
        if (a) unix_put(a);
        if (b) unix_put(b);
        return -ENOMEM;
    }

    unix_pair(a, b);

    struct file *fa = sock_file_new(&a->sock, type), *fb = sock_file_new(&b->sock, type);
    if (!fa || !fb) {
        if (fa) vfs_close(fa);
        if (fb) vfs_close(fb);
        return -ENOMEM;
    }

    struct files *files = current_task()->files;
    int fds[2];

    fds[0] = fd_install(files, fa, type & SOCK_CLOEXEC, 0);
    if (fds[0] < 0) {
        vfs_close(fa);
        vfs_close(fb);
        return fds[0];
    }

    fds[1] = fd_install(files, fb, type & SOCK_CLOEXEC, 0);
    if (fds[1] < 0) {
        sys_close(fds[0]);
        vfs_close(fb);
        return fds[1];
    }

    int err = copy_to_user(usv, fds, sizeof(fds));
    if (err) {
        sys_close(fds[0]);
        sys_close(fds[1]);
    }

    return err;
}

/* ---- addresses ---- */

/* a sockaddr_un from user memory. path: the absolute path of a pathname socket (the caller frees it), else NULL */
static int addr_from_user(long uaddr, long ulen, struct unix_addr *a, char **path) {
    struct sockaddr_un sa;

    *path = NULL;

    if (ulen < (long)sizeof(uint16_t) || ulen > (long)sizeof(sa)) return -EINVAL;

    memset(&sa, 0, sizeof(sa));

    int err = copy_from_user(&sa, uaddr, ulen);
    if (err) return err;

    if (sa.sun_family != AF_UNIX) return -EAFNOSUPPORT;

    size_t n = ulen - sizeof(uint16_t);
    memset(a, 0, sizeof(*a));

    if (n == 0) return 0; /* a=len 0: the caller sees an unnamed address */

    if (sa.sun_path[0] == '\0') { /* abstract: every byte is the name */
        a->len = n;
        memcpy(a->name, sa.sun_path, n);
        return 0;
    }

    n = strnlen(sa.sun_path, n);
    a->len = n;
    memcpy(a->name, sa.sun_path, n);

    char tmp[UNIX_PATH_MAX + 1];

    memcpy(tmp, sa.sun_path, n);
    tmp[n] = '\0';

    return path_from_kernel(tmp, path);
}

static bool is_abstract(const struct unix_addr *a) {
    return a->len > 0 && a->name[0] == '\0';
}

/* the name of a socket for a user: its family, and the bytes of it. a path has its nul counted. as many as fit in room,
 * and *total is how many there are */
static int put_addr(long uaddr, int32_t room, const struct unix_addr *a, bool named, int32_t *total) {
    struct sockaddr_un sa;

    if (room < 0) return -EINVAL;

    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;

    *total = sizeof(uint16_t);

    if (named) {
        memcpy(sa.sun_path, a->name, a->len);
        *total += a->len + (is_abstract(a) ? 0 : 1);
    }

    int32_t n = room < *total ? room : *total;

    return n ? copy_to_user(uaddr, &sa, n) : 0;
}

/* and the length is in user memory too, read first and written after */
static int addr_to_user(long uaddr, long ulenp, const struct unix_addr *a, bool named) {
    int32_t room, total;

    int err = copy_from_user(&room, ulenp, sizeof(room));
    if (err) return err;

    if ((err = put_addr(uaddr, room, a, named, &total))) return err;

    return copy_to_user(ulenp, &total, sizeof(total));
}

static bool name_of(struct usock *sk, struct unix_addr *out) {
    uint64_t f = spin_lock_irqsave(&sk->lock);
    bool named = sk->has_addr;

    if (named) *out = sk->addr;

    spin_unlock_irqrestore(&sk->lock, f);
    return named;
}

/* the socket a name stands for, referenced. a path is a socket file; an abstract name is in a list */
static struct usock *find_target(const struct unix_addr *a, const char *path, int *err) {
    struct usock *sk = NULL;

    if (!path) {
        sk = unix_lookup_abstract(a);
        *err = sk ? 0 : -ECONNREFUSED;
        return sk;
    }

    struct file *pf;

    *err = vfs_open(path, O_PATH, 0, &pf);
    if (*err) return NULL;

    if (!S_ISSOCK(pf->inode->mode) || pf->inode->ops != &sock_ops ||
        ((struct sock *)pf->inode->priv)->ops != &unix_proto_ops) {
        *err = -ECONNREFUSED;
    } else {
        sk = pf->inode->priv;
        unix_get(sk);
    }

    vfs_close(pf);
    return sk;
}

/* ---- bind, listen, connect, accept ---- */

static int unix_op_bind(struct file *f, long uaddr, long ulen) {
    struct usock *sk = usock_of(f);
    struct unix_addr a;
    char *path;
    int err;

    if (sk->netlink) return 0; /* a sockaddr_nl: the groups it would hear are of no use here */

    if ((err = addr_from_user(uaddr, ulen, &a, &path))) return err;

    if (name_of(sk, &(struct unix_addr){ 0 })) {
        err = -EINVAL; /* bound already */
        goto out_path;
    }

    if (a.len == 0) {
        err = unix_bind(sk, NULL, true, true); /* just the family: a name of its own */
    } else if (path) {
        /* the socket's inode gets a name in the file system. a name that is there is an address in use */
        f->inode->mode = S_IFSOCK | (0777 & ~current_task()->files->umask);

        err = vfs_link_inode(path, f->inode);
        if (err == -EEXIST) err = -EADDRINUSE;
        if (!err) err = unix_bind(sk, &a, false, false);
    } else {
        err = unix_bind(sk, &a, true, false);
    }

out_path:
    kfree(path);
    return err;
}

static int unix_op_listen(struct file *f, long backlog) {
    return unix_listen(usock_of(f), backlog);
}

static int unix_op_connect(struct file *f, long uaddr, long ulen) {
    struct usock *sk = usock_of(f);
    struct unix_addr a;
    char *path;
    long err;

    /* a datagram socket can be pointed at nothing again: the family AF_UNSPEC */
    uint16_t family;

    if (sk->type == SOCK_DGRAM && ulen >= 2 && copy_from_user(&family, uaddr, sizeof(family)) == 0 && family == 0) {
        uint64_t fl = spin_lock_irqsave(&sk->lock);
        struct usock *old = sk->peer;
        sk->peer = NULL;
        spin_unlock_irqrestore(&sk->lock, fl);

        if (old) unix_put(old);
        return 0;
    }

    if ((err = addr_from_user(uaddr, ulen, &a, &path)) == 0) {
        if (a.len == 0) {
            err = -EINVAL;
        } else {
            int ferr;
            struct usock *target = find_target(&a, path, &ferr);

            err = ferr;

            if (target) {
                err = unix_connect(sk, target, f->flags & O_NONBLOCK);
                unix_put(target);
            }
        }

        kfree(path);
    }

    return err;
}

static long unix_op_accept(struct file *f, long uaddr, long ulenp, long flags) {
    struct usock *sk = usock_of(f);
    int err;
    struct usock *srv = unix_accept(sk, f->flags & O_NONBLOCK, &err);

    if (!srv) return err;

    /* who connected: the other end of the new one */
    struct unix_addr a;
    bool named = false;

    uint64_t fl = spin_lock_irqsave(&srv->lock);
    struct usock *client = srv->peer;

    if (client) unix_get(client);

    spin_unlock_irqrestore(&srv->lock, fl);

    if (client) {
        named = name_of(client, &a);
        unix_put(client);
    }

    struct file *nf = sock_file_new(&srv->sock, flags);
    if (!nf) return -ENOMEM;

    int nfd = fd_install(current_task()->files, nf, flags & SOCK_CLOEXEC, 0);
    if (nfd < 0) {
        vfs_close(nf);
        return nfd;
    }

    if (uaddr && ulenp && (err = addr_to_user(uaddr, ulenp, &a, named))) {
        sys_close(nfd);
        return err;
    }

    return nfd;
}

long sys_accept4(long fd, long uaddr, long ulenp, long flags) {
    if (flags & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)) return -EINVAL;

    struct file *f = fd_get(fd);
    if (!f) return -EBADF;

    struct sock *sk = sock_of(f);
    long ret = sk ? sk->ops->accept(f, uaddr, ulenp, flags) : -ENOTSOCK;

    vfs_close(f);
    return ret;
}

long sys_accept(long fd, long uaddr, long ulenp) {
    return sys_accept4(fd, uaddr, ulenp, 0);
}

long sys_bind(long fd, long uaddr, long ulen) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;

    struct sock *sk = sock_of(f);
    long err = sk ? sk->ops->bind(f, uaddr, ulen) : -ENOTSOCK;

    vfs_close(f);
    return err;
}

long sys_listen(long fd, long backlog) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;

    struct sock *sk = sock_of(f);
    long err = sk ? sk->ops->listen(f, backlog) : -ENOTSOCK;

    vfs_close(f);
    return err;
}

long sys_connect(long fd, long uaddr, long ulen) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;

    struct sock *sk = sock_of(f);
    long err = sk ? sk->ops->connect(f, uaddr, ulen) : -ENOTSOCK;

    vfs_close(f);
    return err;
}

/* struct sockaddr_nl: the family, a pad, the port and the groups */
struct sockaddr_nl_user {
    uint16_t family;
    uint16_t pad;
    uint32_t pid;
    uint32_t groups;
};

static long unix_op_getname(struct file *f, long uaddr, long ulenp, bool peer) {
    struct usock *sk = usock_of(f);
    struct unix_addr a;

    if (peer) {
        uint64_t fl = spin_lock_irqsave(&sk->lock);
        struct usock *other = sk->peer;

        if (other) unix_get(other);

        spin_unlock_irqrestore(&sk->lock, fl);

        if (!other) return -ENOTCONN;

        bool named = name_of(other, &a);
        long err = addr_to_user(uaddr, ulenp, &a, named);

        unix_put(other);
        return err;
    }

    if (sk->netlink) {
        struct sockaddr_nl_user nl = { .family = AF_NETLINK, .pid = sys_getpid() };
        int32_t room, total = sizeof(nl);
        long err = copy_from_user(&room, ulenp, sizeof(room));

        if (!err) err = copy_to_user(uaddr, &nl, room < total ? (size_t)(room > 0 ? room : 0) : sizeof(nl));
        if (!err) err = copy_to_user(ulenp, &total, sizeof(total));

        return err;
    }

    bool named = name_of(sk, &a);

    return addr_to_user(uaddr, ulenp, &a, named);
}

long sys_getsockname(long fd, long uaddr, long ulenp) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;

    struct sock *sk = sock_of(f);
    long err = sk ? sk->ops->getname(f, uaddr, ulenp, false) : -ENOTSOCK;

    vfs_close(f);
    return err;
}

long sys_getpeername(long fd, long uaddr, long ulenp) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;

    struct sock *sk = sock_of(f);
    long err = sk ? sk->ops->getname(f, uaddr, ulenp, true) : -ENOTSOCK;

    vfs_close(f);
    return err;
}

static int unix_op_shutdown(struct file *f, long how) {
    return unix_shutdown(usock_of(f), how);
}

long sys_shutdown(long fd, long how) {
    if (how < SHUT_RD || how > SHUT_RDWR) return -EINVAL;

    struct file *f = fd_get(fd);
    if (!f) return -EBADF;

    struct sock *sk = sock_of(f);
    long err = sk ? sk->ops->shutdown(f, how) : -ENOTSOCK;

    vfs_close(f);
    return err;
}

/* ---- messages ---- */

/* the files and the credentials a message carries, out of its control data. the references are taken here */
static int parse_control(struct msghdr *mh, struct unix_msg *m) {
    if (!mh->msg_controllen) return 0;
    if (mh->msg_controllen > CONTROL_MAX) return -ENOBUFS;

    struct kbuf kb;
    if (!kbuf_alloc(&kb, mh->msg_controllen)) return -ENOMEM;

    int err = copy_from_user(kb.p, mh->msg_control, mh->msg_controllen);

    for (size_t off = 0; !err && off + sizeof(struct cmsghdr) <= mh->msg_controllen;) {
        struct cmsghdr *c = (struct cmsghdr *)((uint8_t *)kb.p + off);

        if (c->cmsg_len < sizeof(*c) || off + c->cmsg_len > mh->msg_controllen) {
            err = -EINVAL;
            break;
        }

        size_t dlen = c->cmsg_len - sizeof(*c);
        const uint8_t *data = (const uint8_t *)(c + 1);

        if (c->cmsg_level != SOL_SOCKET) {
            err = -EINVAL;
        } else if (c->cmsg_type == SCM_RIGHTS) {
            if (dlen % sizeof(int32_t)) {
                err = -EINVAL;
                break;
            }

            for (size_t i = 0; i < dlen / sizeof(int32_t); i++) {
                int32_t fd;

                memcpy(&fd, data + i * sizeof(fd), sizeof(fd));

                if (m->nfds == SCM_MAX_FD) {
                    err = -EINVAL;
                    break;
                }

                struct file *f = fd_get(fd);
                if (!f) {
                    err = -EBADF;
                    break;
                }

                m->fds[m->nfds++] = f;
            }
        } else if (c->cmsg_type == SCM_CREDENTIALS) {
            if (dlen < sizeof(struct ucred)) {
                err = -EINVAL;
                break;
            }

            struct ucred cred;

            memcpy(&cred, data, sizeof(cred));

            /* only who it really is */
            if (cred.pid != sys_getpid() || cred.uid != 0 || cred.gid != 0) {
                err = -EPERM;
            } else {
                m->cred = cred;
                m->has_cred = true;
            }
        } else {
            err = -EINVAL;
        }

        off += CMSG_ALIGN(c->cmsg_len);
    }

    kbuf_free(&kb);

    if (err) unix_msg_drop(m);
    return err;
}

/* a stream is sent in pieces: each is copied out of the iovecs into one kernel buffer and sent whole */
static long unix_op_sendmsg(struct file *f, struct msghdr *mh, const struct iovec *iov, unsigned flags) {
    struct usock *sk = usock_of(f);
    bool nonblock = (f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT);
    struct unix_msg m = { 0 };
    struct kbuf kb = { 0 };
    long err = 0, sent = 0;
    size_t total = 0;

    for (size_t i = 0; i < mh->msg_iovlen; i++) {
        if (iov[i].len > 0x7fffffff || total + iov[i].len > 0x7fffffff) return -EINVAL;
        total += iov[i].len;
    }

    bool stream = sk->type == SOCK_STREAM;

    if (!stream && total > UNIX_MSG_MAX) return -EMSGSIZE;

    /* where a datagram goes, if it says */
    if (mh->msg_name && sk->type == SOCK_DGRAM) {
        struct unix_addr a;
        char *path;

        if ((err = addr_from_user(mh->msg_name, mh->msg_namelen, &a, &path))) return err;

        if (a.len == 0) {
            kfree(path);
            return -EINVAL;
        }

        int ferr;

        m.to = find_target(&a, path, &ferr);
        kfree(path);

        if (!m.to) return ferr;
    }

    if ((err = parse_control(mh, &m))) goto out;

    if (!kbuf_alloc(&kb, total < UNIX_CHUNK ? total : UNIX_CHUNK)) {
        err = -ENOMEM;
        goto out;
    }

    size_t vec = 0, voff = 0;

    do {
        size_t n = total - sent, chunk = n < UNIX_CHUNK ? n : UNIX_CHUNK;

        /* the chunk, gathered */
        for (size_t got = 0; got < chunk;) {
            while (voff == iov[vec].len) {
                vec++;
                voff = 0;
            }

            size_t take = iov[vec].len - voff;
            if (take > chunk - got) take = chunk - got;

            if ((err = copy_from_user((uint8_t *)kb.p + got, iov[vec].base + voff, take))) goto out;

            got += take;
            voff += take;
        }

        m.data = kb.p;
        m.len = chunk;

        long r = unix_send(sk, &m, nonblock);
        if (r < 0) {
            err = r;
            break;
        }

        sent += r;
    } while (sent < (long)total);

out:
    kbuf_free(&kb);
    unix_msg_drop(&m);
    if (m.to) unix_put(m.to);

    if (err == -EPIPE && !(flags & MSG_NOSIGNAL)) send_sigpipe();

    return sent ? sent : err;
}

/* the control data of what was received: the files, installed here, and who sent it */
static int build_control(struct msghdr *mh, struct usock *sk, struct unix_rx *rx, unsigned flags, int *msg_flags) {
    size_t room = mh->msg_controllen, used = 0;
    struct kbuf kb = { 0 };

    if (!room || (!rx->nfds && !(sk->passcred && rx->has_cred))) {
        unix_rx_drop(rx);
        mh->msg_controllen = 0;
        return 0;
    }

    if (room > CONTROL_MAX) room = CONTROL_MAX;
    if (!kbuf_alloc(&kb, room)) {
        unix_rx_drop(rx);
        return -ENOMEM;
    }

    memset(kb.p, 0, room);

    if (rx->nfds) {
        size_t fit = room >= sizeof(struct cmsghdr) ? (room - sizeof(struct cmsghdr)) / sizeof(int32_t) : 0;
        unsigned put = 0;
        struct files *files = current_task()->files;
        struct cmsghdr *c = kb.p;
        int32_t *fds = (int32_t *)(c + 1);

        for (unsigned i = 0; i < rx->nfds; i++) {
            if (i < fit) {
                int fd = fd_install(files, rx->fds[i], flags & MSG_CMSG_CLOEXEC, 0);

                if (fd >= 0) {
                    fds[put++] = fd;
                    continue;
                }
            }

            vfs_close(rx->fds[i]);
            *msg_flags |= MSG_CTRUNC;
        }

        rx->nfds = 0;

        if (put) {
            c->cmsg_len = CMSG_LEN(put * sizeof(int32_t));
            c->cmsg_level = SOL_SOCKET;
            c->cmsg_type = SCM_RIGHTS;
            used = CMSG_ALIGN(c->cmsg_len);
        }
    }

    if (sk->passcred && rx->has_cred) {
        if (used + CMSG_SPACE(sizeof(struct ucred)) <= room) {
            struct cmsghdr *c = (struct cmsghdr *)((uint8_t *)kb.p + used);

            c->cmsg_len = CMSG_LEN(sizeof(struct ucred));
            c->cmsg_level = SOL_SOCKET;
            c->cmsg_type = SCM_CREDENTIALS;
            memcpy(c + 1, &rx->cred, sizeof(rx->cred));
            used += CMSG_SPACE(sizeof(struct ucred));
        } else {
            *msg_flags |= MSG_CTRUNC;
        }
    }

    int err = used ? copy_to_user(mh->msg_control, kb.p, used) : 0;

    mh->msg_controllen = used;
    kbuf_free(&kb);
    return err;
}

static long unix_op_recvmsg(struct file *f, struct msghdr *mh, const struct iovec *iov, unsigned flags) {
    struct usock *sk = usock_of(f);
    bool nonblock = (f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT);
    struct unix_rx rx = { 0 };
    struct kbuf kb = { 0 };
    long err = 0, got = 0;
    size_t cap = 0;
    int msg_flags = 0;

    for (size_t i = 0; i < mh->msg_iovlen; i++) {
        if (iov[i].len > 0x7fffffff || cap + iov[i].len > 0x7fffffff) return -EINVAL;
        cap += iov[i].len;
    }

    bool stream = sk->type == SOCK_STREAM;
    size_t want = cap < UNIX_MSG_MAX ? cap : UNIX_MSG_MAX;

    if (!kbuf_alloc(&kb, want)) return -ENOMEM;

    /* a stream read with MSG_WAITALL comes back with all it asked for, or at the end */
    do {
        rx.buf = (uint8_t *)kb.p + got;
        rx.cap = want - got;

        long r = unix_recv(sk, &rx, flags, nonblock);

        if (r < 0) {
            err = got ? 0 : r;
            break;
        }

        got += r;

        if (r == 0 || !stream || !(flags & MSG_WAITALL) || (flags & MSG_PEEK)) break;

        /* the files of this piece go out below with the data; a longer wait would hold them */
        if (rx.nfds) break;
    } while ((size_t)got < want);

    if (err) goto out;

    if (rx.trunc) msg_flags |= MSG_TRUNC;

    /* scatter */
    {
        size_t off = 0;

        for (size_t i = 0; i < mh->msg_iovlen && off < (size_t)got; i++) {
            size_t n = iov[i].len < (size_t)got - off ? iov[i].len : (size_t)got - off;

            if ((err = copy_to_user(iov[i].base, (uint8_t *)kb.p + off, n))) goto out;

            off += n;
        }
    }

    /* who sent it */
    if (mh->msg_name) {
        int32_t total = 0;

        if (rx.has_from && (err = put_addr(mh->msg_name, mh->msg_namelen, &rx.from, true, &total))) goto out;

        mh->msg_namelen = total;
    }

    if ((err = build_control(mh, sk, &rx, flags, &msg_flags))) goto out;

    mh->msg_flags = msg_flags;

    /* a datagram says how long it was, if asked */
    if (!stream && (flags & MSG_TRUNC)) got = rx.full;

out:
    kbuf_free(&kb);
    unix_rx_drop(&rx);
    return err ? err : got;
}

/* the iovec array of a message, into the kernel */
static int iov_from_user(const struct msghdr *mh, struct kbuf *kb) {
    if (mh->msg_iovlen > IOV_MAX) return -EMSGSIZE;
    if (!kbuf_alloc(kb, mh->msg_iovlen * sizeof(struct iovec))) return -ENOMEM;

    int err = mh->msg_iovlen ? copy_from_user(kb->p, mh->msg_iov, mh->msg_iovlen * sizeof(struct iovec)) : 0;
    if (err) kbuf_free(kb);

    return err;
}

/* sendmsg and recvmsg of one header in user memory */
static long msg_op(long fd, struct msghdr *mh, unsigned flags, bool send) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;

    struct sock *sk = sock_of(f);
    struct kbuf iov;
    long ret;

    if (!sk) {
        vfs_close(f);
        return -ENOTSOCK;
    }

    if ((ret = iov_from_user(mh, &iov))) {
        vfs_close(f);
        return ret;
    }

    ret = send ? sk->ops->sendmsg(f, mh, iov.p, flags) : sk->ops->recvmsg(f, mh, iov.p, flags);

    kbuf_free(&iov);
    vfs_close(f);
    return ret;
}

long sys_sendmsg(long fd, long umsg, long flags) {
    struct msghdr mh;

    if (copy_from_user(&mh, umsg, sizeof(mh)) != 0) return -EFAULT;

    return msg_op(fd, &mh, flags, true);
}

long sys_recvmsg(long fd, long umsg, long flags) {
    struct msghdr mh;

    if (copy_from_user(&mh, umsg, sizeof(mh)) != 0) return -EFAULT;

    long ret = msg_op(fd, &mh, flags, false);
    if (ret < 0) return ret;

    return copy_to_user(umsg, &mh, sizeof(mh)) ? -EFAULT : ret;
}

/* sendto and recvfrom: a message of one buffer */
long sys_sendto(long fd, long ubuf, long len, long flags, long uaddr, long ulen) {
    struct iovec iov = { .base = ubuf, .len = len };
    struct msghdr mh = { .msg_name = uaddr, .msg_namelen = uaddr ? ulen : 0, .msg_iovlen = 1 };

    if (len < 0) return -EINVAL;

    struct file *f = fd_get(fd);
    if (!f) return -EBADF;

    struct sock *sk = sock_of(f);
    long ret = sk ? sk->ops->sendmsg(f, &mh, &iov, flags) : -ENOTSOCK;

    vfs_close(f);
    return ret;
}

long sys_recvfrom(long fd, long ubuf, long len, long flags, long uaddr, long ulenp) {
    struct iovec iov = { .base = ubuf, .len = len };
    struct msghdr mh = { .msg_iovlen = 1 };
    int32_t ulen = 0;

    if (len < 0) return -EINVAL;

    if (uaddr) {
        if (!ulenp || copy_from_user(&ulen, ulenp, sizeof(ulen)) != 0) return -EFAULT;

        mh.msg_name = uaddr;
        mh.msg_namelen = ulen;
    }

    struct file *f = fd_get(fd);
    if (!f) return -EBADF;

    struct sock *sk = sock_of(f);
    long ret = sk ? sk->ops->recvmsg(f, &mh, &iov, flags) : -ENOTSOCK;

    vfs_close(f);

    if (ret >= 0 && uaddr && copy_to_user(ulenp, &mh.msg_namelen, sizeof(mh.msg_namelen)) != 0) return -EFAULT;
    return ret;
}

#define UIO_MAXVLEN 1024

/* several at once: one message each, and the count that went */
long sys_sendmmsg(long fd, long uvec, long vlen, long flags) {
    long done = 0;

    if (vlen > UIO_MAXVLEN) vlen = UIO_MAXVLEN;

    for (; done < vlen; done++) {
        struct mmsghdr mm;

        if (copy_from_user(&mm, uvec + done * sizeof(mm), sizeof(mm)) != 0) return done ? done : -EFAULT;

        long r = msg_op(fd, &mm.msg_hdr, flags, true);
        if (r < 0) return done ? done : r;

        mm.msg_len = r;

        if (copy_to_user(uvec + done * sizeof(mm), &mm, sizeof(mm)) != 0) return done ? done : -EFAULT;
    }

    return done;
}

long sys_recvmmsg(long fd, long uvec, long vlen, long flags, long utimeout) {
    long done = 0;

    (void)utimeout; /* the wait for each is the socket's own */

    if (vlen > UIO_MAXVLEN) vlen = UIO_MAXVLEN;

    for (; done < vlen; done++) {
        struct mmsghdr mm;

        if (copy_from_user(&mm, uvec + done * sizeof(mm), sizeof(mm)) != 0) return done ? done : -EFAULT;

        /* after the first, only what is there: a vector is not a reason to wait */
        long r = msg_op(fd, &mm.msg_hdr, done ? flags | MSG_DONTWAIT : flags, false);
        if (r < 0) return done ? done : r;

        mm.msg_len = r;

        if (copy_to_user(uvec + done * sizeof(mm), &mm, sizeof(mm)) != 0) return done ? done : -EFAULT;
    }

    return done;
}

/* ---- options ---- */

#define OPT_MIN_BUF 4608
#define OPT_MAX_BUF (8 << 20)

struct timeval_user {
    int64_t sec;
    int64_t usec;
};

/* the options that only have to be given back as they were set: a bit for each */
static bool opt_is_flag(long name) {
    switch (name) {
        case SO_DEBUG:
        case SO_REUSEADDR:
        case SO_DONTROUTE:
        case SO_BROADCAST:
        case SO_KEEPALIVE:
        case SO_OOBINLINE:
        case SO_NO_CHECK:
        case SO_REUSEPORT:
        case SO_PASSSEC:
            return true;
    }

    return false;
}

static long unix_op_setsockopt(struct file *f, long level, long name, long uval, long len) {
    struct usock *sk = usock_of(f);
    long err = 0;
    int32_t v = 0;

    if (level != SOL_SOCKET) return -ENOPROTOOPT;

    if (name == SO_RCVTIMEO || name == SO_SNDTIMEO) {
        struct timeval_user tv;

        if (len < (long)sizeof(tv)) err = -EINVAL;
        else if (copy_from_user(&tv, uval, sizeof(tv))) err = -EFAULT;
        else if (tv.sec < 0 || tv.usec < 0 || tv.usec >= 1000000) err = -EDOM;

        if (!err) {
            uint64_t freq = this_cpu_read(tsc_freq_hz);
            uint64_t ticks = tv.sec * freq + tv.usec * freq / 1000000;

            if (name == SO_RCVTIMEO) sk->rcvtimeo = ticks;
            else                     sk->sndtimeo = ticks;
        }
    } else if (name == SO_LINGER) {
        if (len < 8) err = -EINVAL; /* taken and not used: a close does not wait */
    } else {
        if (len < (long)sizeof(v)) err = -EINVAL;
        else if (copy_from_user(&v, uval, sizeof(v))) err = -EFAULT;

        if (!err) {
            uint64_t fl = spin_lock_irqsave(&sk->lock);

            if (name == SO_SNDBUF || name == SO_RCVBUF || name == SO_SNDBUFFORCE || name == SO_RCVBUFFORCE) {
                int64_t size = 2 * (int64_t)v; /* as linux: it is told back doubled, for the bookkeeping */

                if (size < OPT_MIN_BUF) size = OPT_MIN_BUF;
                if (size > OPT_MAX_BUF * 2) size = OPT_MAX_BUF * 2;

                if (name == SO_SNDBUF || name == SO_SNDBUFFORCE) sk->sndbuf = size;
                else                                             sk->rcvbuf = size;
            } else if (name == SO_PASSCRED) {
                sk->passcred = v != 0;
            } else if (opt_is_flag(name)) {
                if (v) sk->flags |= 1u << name;
                else   sk->flags &= ~(1u << name);
            } else if (sk->netlink && (name == SO_ATTACH_FILTER || name == SO_ATTACH_BPF)) {
                /* a filter for messages that do not come */
            } else if (name != SO_PRIORITY && name != SO_RCVLOWAT && name != SO_SNDLOWAT) {
                err = -ENOPROTOOPT;
            }

            spin_unlock_irqrestore(&sk->lock, fl);
        }
    }

    return err;
}

long sys_setsockopt(long fd, long level, long name, long uval, long len) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;

    struct sock *sk = sock_of(f);
    long err = sk ? sk->ops->setsockopt(f, level, name, uval, len) : -ENOTSOCK;

    vfs_close(f);
    return err;
}

static long unix_op_getsockopt(struct file *f, long level, long name, long uval, long ulenp) {
    struct usock *sk = usock_of(f);
    int32_t ulen;
    long err;

    if (level != SOL_SOCKET) return -ENOPROTOOPT;

    if ((err = copy_from_user(&ulen, ulenp, sizeof(ulen)))) return err;
    if (ulen < 0) return -EINVAL;

    union {
        int32_t              i;
        struct ucred         cred;
        struct timeval_user  tv;
        int32_t              linger[2];
    } out = { 0 };

    size_t size = sizeof(int32_t);

    uint64_t fl = spin_lock_irqsave(&sk->lock);

    switch (name) {
        case SO_TYPE:        out.i = sk->type; break;
        case SO_ERROR:       out.i = 0; break;
        case SO_ACCEPTCONN:  out.i = sk->listening; break;
        case SO_SNDBUF:
        case SO_SNDBUFFORCE: out.i = sk->sndbuf; break;
        case SO_RCVBUF:
        case SO_RCVBUFFORCE: out.i = sk->rcvbuf; break;
        case SO_PASSCRED:    out.i = sk->passcred; break;
        case SO_DOMAIN:      out.i = AF_UNIX; break;
        case SO_PROTOCOL:    out.i = 0; break;
        case SO_PRIORITY:    out.i = 0; break;
        case SO_RCVLOWAT:
        case SO_SNDLOWAT:    out.i = 1; break;
        case SO_LINGER:      size = sizeof(out.linger); break; /* off */

        case SO_PEERCRED:
            size = sizeof(struct ucred);

            /* a socket that never was connected has nobody: -1, as linux says it */
            out.cred = sk->peercred.pid ? sk->peercred : (struct ucred){ 0, (uint32_t)-1, (uint32_t)-1 };
            break;

        case SO_RCVTIMEO:
        case SO_SNDTIMEO: {
            uint64_t freq = this_cpu_read(tsc_freq_hz), t = name == SO_RCVTIMEO ? sk->rcvtimeo : sk->sndtimeo;

            size = sizeof(out.tv);
            out.tv.sec = t / freq;
            out.tv.usec = t % freq * 1000000 / freq;
            break;
        }

        default:
            if (opt_is_flag(name)) out.i = (sk->flags >> name) & 1;
            else                   err = -ENOPROTOOPT;
    }

    spin_unlock_irqrestore(&sk->lock, fl);

    if (err) return err;

    {
        int32_t n = ulen < (int32_t)size ? ulen : (int32_t)size, outlen = size;

        if ((n && (err = copy_to_user(uval, &out, n))) || (err = copy_to_user(ulenp, &outlen, sizeof(outlen)))) return err;
    }

    return 0;
}

long sys_getsockopt(long fd, long level, long name, long uval, long ulenp) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;

    struct sock *sk = sock_of(f);
    long err = sk ? sk->ops->getsockopt(f, level, name, uval, ulenp) : -ENOTSOCK;

    vfs_close(f);
    return err;
}

const struct proto_ops unix_proto_ops = {
    .bind       = unix_op_bind,
    .connect    = unix_op_connect,
    .listen     = unix_op_listen,
    .accept     = unix_op_accept,
    .getname    = unix_op_getname,
    .shutdown   = unix_op_shutdown,
    .sendmsg    = unix_op_sendmsg,
    .recvmsg    = unix_op_recvmsg,
    .setsockopt = unix_op_setsockopt,
    .getsockopt = unix_op_getsockopt,
    .read       = unix_op_read,
    .write      = unix_op_write,
    .poll       = unix_op_poll,
    .ioctl      = unix_op_ioctl,
    .release    = unix_op_release,
    .put        = unix_op_put,
};
