#include <serika/errno.h>
#include <serika/fcntl.h>
#include <serika/list.h>
#include <serika/lock.h>
#include <serika/mm.h>
#include <serika/poll.h>
#include <serika/sched.h>
#include <serika/signal.h>
#include <serika/slab.h>
#include <serika/string.h>
#include <serika/syscall.h>
#include <serika/vfs.h>

#include <asm/arch.h>

#define EPOLL_CLOEXEC O_CLOEXEC

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

#define EPOLLIN        0x001
#define EPOLLPRI       0x002
#define EPOLLOUT       0x004
#define EPOLLERR       0x008
#define EPOLLHUP       0x010
#define EPOLLRDNORM    0x040
#define EPOLLWRNORM    0x100
#define EPOLLRDHUP     0x2000
#define EPOLLEXCLUSIVE (1u << 28)
#define EPOLLWAKEUP    (1u << 29)
#define EPOLLONESHOT   (1u << 30)
#define EPOLLET        (1u << 31)

/* the bits of an item's events that are no event: what is left of them says whether it is on */
#define EP_PRIVATE (EPOLLEXCLUSIVE | EPOLLWAKEUP | EPOLLONESHOT | EPOLLET)

struct epoll_event {
    uint32_t events;
    uint64_t data;
} __attribute__((packed)); /* x86_64's, 12 bytes */

/* a file it watches, by fd. the file is only compared, never touched: it has no reference of ours (a socket must
 * see its last close even if an epoll still lists it), so it may be gone, or its fd number another file's */
struct ep_item {
    list_node node;
    int       fd;
    struct file *file;
    uint32_t  events;
    uint64_t  data;
    bool      nested; /* an epoll itself */
};

/* no waiting state: epoll_wait() is poll_core() over a copy of the items, and poll() asks the items too */
struct epoll {
    spinlock_t lock; /* the items. a file's poll() runs under it: nothing in one takes an epoll's lock but a nested epoll */
    list_node  items;
    unsigned   count;
};

static const struct inode_ops epoll_ops;

static struct epoll *epoll_of(struct inode *inode) {
    return inode->priv;
}

/* lock held */
static struct ep_item *ep_find(struct epoll *ep, int fd) {
    for (list_node *n = ep->items.next; n != &ep->items; n = n->next) {
        struct ep_item *it = container_of(n, struct ep_item, node);

        if (it->fd == fd) return it;
    }

    return NULL;
}

/* lock held */
static void ep_remove(struct epoll *ep, struct ep_item *it) {
    list_del(&it->node);
    ep->count--;
    kfree(it);
}

/* the poll() events an item asks for */
static int16_t poll_events(uint32_t events) {
    int16_t pe = events & (EPOLLIN | EPOLLPRI | EPOLLOUT);

    if (events & EPOLLRDNORM) pe |= POLLIN;
    if (events & EPOLLWRNORM) pe |= POLLOUT;

    return pe;
}

/* what poll() found, as epoll says it, of what the item asked for. error and hangup are always told */
static uint32_t ep_events(uint32_t revents, uint32_t events) {
    uint32_t ev = revents & (POLLIN | POLLPRI | POLLOUT | POLLERR | POLLHUP);

    if (revents & POLLIN)  ev |= EPOLLRDNORM;
    if (revents & POLLOUT) ev |= EPOLLWRNORM;
    if (revents & POLLHUP) ev |= EPOLLRDHUP;

    return ev & (events | EPOLLERR | EPOLLHUP);
}

/* epoll in epoll: ready when something in it is. what it waits on is registered as far as the poll table has room */
static uint32_t epoll_poll(struct inode *inode, uint32_t fflags, struct poll_table *pt) {
    struct epoll *ep = epoll_of(inode);
    uint32_t mask = 0;

    (void)fflags;

    uint64_t flags = spin_lock_irqsave(&ep->lock);

    for (list_node *n = ep->items.next; n != &ep->items && !mask; n = n->next) {
        struct ep_item *it = container_of(n, struct ep_item, node);

        if (!(it->events & ~EP_PRIVATE)) continue;

        struct file *f = fd_get(it->fd);
        if (!f) continue;

        if (f == it->file) {
            uint32_t ready = vfs_poll(f, pt);

            if (ep_events(ready, it->events)) mask = POLLIN;
        }

        vfs_close(f);
    }

    spin_unlock_irqrestore(&ep->lock, flags);
    return mask;
}

static void epoll_evict(struct inode *inode) {
    struct epoll *ep = epoll_of(inode);

    while (!list_empty(&ep->items)) ep_remove(ep, list_first(&ep->items, struct ep_item, node));

    kfree(ep);
}

static const struct inode_ops epoll_ops = {
    .poll  = epoll_poll,
    .evict = epoll_evict,
};

long sys_epoll_create1(long flags) {
    if (flags & ~EPOLL_CLOEXEC) return -EINVAL;

    struct epoll *ep = kmalloc(sizeof(*ep));
    if (!ep) return -ENOMEM;

    spin_lock_init(&ep->lock);
    list_init(&ep->items);
    ep->count = 0;

    struct inode *inode = inode_alloc(&epoll_ops, 0600);
    if (!inode) {
        kfree(ep);
        return -ENOMEM;
    }

    inode->priv = ep; /* evict frees it, from here on */

    return anon_fd(inode, O_RDWR, flags & EPOLL_CLOEXEC);
}

long sys_epoll_create(long size) {
    if (size <= 0) return -EINVAL;

    return sys_epoll_create1(0);
}

/* the file is referenced: vfs_close() it */
static int epoll_get(long fd, struct file **out) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;

    if (f->inode->ops != &epoll_ops) {
        vfs_close(f);
        return -EINVAL;
    }

    *out = f;
    return 0;
}

/* an epoll that holds another cannot be put in one: that is as deep as it goes, and no loop */
static bool ep_has_epoll(struct epoll *ep) {
    bool found = false;

    uint64_t flags = spin_lock_irqsave(&ep->lock);

    for (list_node *n = ep->items.next; n != &ep->items; n = n->next)
        if (container_of(n, struct ep_item, node)->nested) found = true;

    spin_unlock_irqrestore(&ep->lock, flags);
    return found;
}

static long ep_modify(struct epoll *ep, long op, int fd, struct file *target, const struct epoll_event *ev) {
    struct ep_item *new = NULL;

    if (op == EPOLL_CTL_ADD && !(new = kmalloc(sizeof(*new)))) return -ENOMEM;

    long ret = 0;
    uint64_t flags = spin_lock_irqsave(&ep->lock);

    struct ep_item *it = ep_find(ep, fd);

    /* the fd was closed and its number came up again: that item is of a file that is gone */
    if (it && it->file != target && op == EPOLL_CTL_ADD) {
        ep_remove(ep, it);
        it = NULL;
    }

    if (op == EPOLL_CTL_ADD) {
        if (it) {
            ret = -EEXIST;
        } else {
            new->fd     = fd;
            new->file   = target;
            new->events = ev->events | EPOLLERR | EPOLLHUP;
            new->data   = ev->data;
            new->nested = target->inode->ops == &epoll_ops;

            list_add_tail(&new->node, &ep->items);
            ep->count++;
            new = NULL;
        }
    } else if (!it || it->file != target) {
        ret = -ENOENT;
    } else if (op == EPOLL_CTL_DEL) {
        ep_remove(ep, it);
    } else {
        it->events = ev->events | EPOLLERR | EPOLLHUP;
        it->data   = ev->data;
    }

    spin_unlock_irqrestore(&ep->lock, flags);

    kfree(new);
    return ret;
}

long sys_epoll_ctl(long epfd, long op, long fd, long uevent) {
    struct epoll_event ev = { 0 };
    struct file *ef;

    if (op != EPOLL_CTL_ADD && op != EPOLL_CTL_DEL && op != EPOLL_CTL_MOD) return -EINVAL;
    if (op != EPOLL_CTL_DEL && copy_from_user(&ev, uevent, sizeof(ev)) != 0) return -EFAULT;

    long ret = epoll_get(epfd, &ef);
    if (ret) return ret;

    struct file *target = fd_get(fd);
    if (!target) {
        vfs_close(ef);
        return -EBADF;
    }

    if (target == ef)                          ret = -EINVAL;
    else if (!vfs_pollable(target))             ret = -EPERM; /* a file that is always ready */
    else if (op == EPOLL_CTL_ADD && target->inode->ops == &epoll_ops && ep_has_epoll(epoll_of(target->inode)))
                                               ret = -ELOOP;
    else                                       ret = ep_modify(epoll_of(ef->inode), op, fd, target, &ev);

    vfs_close(target);
    vfs_close(ef);
    return ret;
}

/* the items that are on, as pollfds with the file each was of. at most max */
static unsigned ep_snapshot(struct epoll *ep, struct pollfd *pfs, struct file **files, unsigned max) {
    unsigned n = 0;

    uint64_t flags = spin_lock_irqsave(&ep->lock);

    for (list_node *l = ep->items.next; l != &ep->items && n < max; l = l->next) {
        struct ep_item *it = container_of(l, struct ep_item, node);

        if (!(it->events & ~EP_PRIVATE)) continue; /* a oneshot that has been told */

        pfs[n] = (struct pollfd){ .fd = it->fd, .events = poll_events(it->events) };
        files[n] = it->file;
        n++;
    }

    spin_unlock_irqrestore(&ep->lock, flags);
    return n;
}

/* what poll_core() found, as events (at most max). an item that was found closed is dropped, and so is one whose
 * fd is another file now. the ones told go last in the list: a full out does not starve the rest */
static unsigned ep_collect(struct epoll *ep, struct pollfd *pfs, struct file **files, unsigned n,
                           struct epoll_event *out, unsigned max) {
    unsigned nout = 0;

    for (unsigned i = 0; i < n && nout < max; i++) {
        if (!pfs[i].revents) continue;

        bool gone = pfs[i].revents & POLLNVAL;

        if (!gone) {
            struct file *f = fd_get(pfs[i].fd);

            gone = f != files[i];
            if (f) vfs_close(f);
        }

        uint64_t flags = spin_lock_irqsave(&ep->lock);

        struct ep_item *it = ep_find(ep, pfs[i].fd);

        if (it && it->file == files[i]) {
            uint32_t ev = gone ? 0 : ep_events(pfs[i].revents, it->events);

            if (gone) {
                ep_remove(ep, it);
            } else if (ev) {
                out[nout++] = (struct epoll_event){ .events = ev, .data = it->data };

                if (it->events & EPOLLONESHOT) it->events &= EP_PRIVATE;

                list_del(&it->node);
                list_add_tail(&it->node, &ep->items);
            }
        }

        spin_unlock_irqrestore(&ep->lock, flags);
    }

    return nout;
}

/* a change of the items by another thread is seen when this one wakes next: the wait is on a copy */
static long ep_wait(long epfd, long uevents, long maxevents, int64_t timeout) {
    struct file *ef;

    if (maxevents <= 0) return -EINVAL;

    long err = epoll_get(epfd, &ef);
    if (err) return err;

    struct epoll *ep = epoll_of(ef->inode);

    uint64_t flags = spin_lock_irqsave(&ep->lock);
    unsigned cap = ep->count;
    spin_unlock_irqrestore(&ep->lock, flags);

    unsigned room = (unsigned long)maxevents < cap ? (unsigned)maxevents : cap;

    struct pollfd *pfs = kmalloc(cap ? cap * sizeof(*pfs) : 1);
    struct file **files = kmalloc(cap ? cap * sizeof(*files) : 1);
    struct epoll_event *out = kmalloc(room ? room * sizeof(*out) : 1);

    long ret = -ENOMEM;
    uint64_t deadline = timeout > 0 ? rdtsc() + timeout : 0;

    if (pfs && files && out) {
        for (;;) {
            unsigned n = ep_snapshot(ep, pfs, files, cap);

            ret = poll_core(pfs, n, timeout);
            if (ret < 0) break;

            ret = ep_collect(ep, pfs, files, n, out, room);

            if (ret || timeout == 0) break;

            if (timeout > 0) {
                uint64_t now = rdtsc();

                timeout = deadline > now ? deadline - now : 0;
            }
        }

        if (ret > 0 && copy_to_user(uevents, out, ret * sizeof(*out)) != 0) ret = -EFAULT;
    }

    kfree(pfs);
    kfree(files);
    kfree(out);

    vfs_close(ef);
    return ret;
}

long sys_epoll_wait(long epfd, long uevents, long maxevents, long timeout_ms) {
    return ep_wait(epfd, uevents, maxevents, timeout_ms < 0 ? -1 : ms_to_tsc(timeout_ms));
}

long sys_epoll_pwait(long epfd, long uevents, long maxevents, long timeout_ms, long usigmask, long sigsetsize) {
    long err;

    if (usigmask && (err = sigmask_swap(usigmask, sigsetsize))) return err;

    long ret = sys_epoll_wait(epfd, uevents, maxevents, timeout_ms);

    if (usigmask) sigmask_unswap(ret);
    return ret;
}

long sys_epoll_pwait2(long epfd, long uevents, long maxevents, long utsp, long usigmask, long sigsetsize) {
    int64_t timeout;

    long err = timeout_from_timespec(utsp, &timeout);
    if (err) return err;

    if (usigmask && (err = sigmask_swap(usigmask, sigsetsize))) return err;

    long ret = ep_wait(epfd, uevents, maxevents, timeout);

    if (usigmask) sigmask_unswap(ret);
    return ret;
}
