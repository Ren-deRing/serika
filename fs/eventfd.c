#include <serika/errno.h>
#include <serika/fcntl.h>
#include <serika/poll.h>
#include <serika/preempt.h>
#include <serika/sched.h>
#include <serika/signal.h>
#include <serika/slab.h>
#include <serika/string.h>
#include <serika/syscall.h>
#include <serika/task.h>
#include <serika/vfs.h>
#include <serika/wait.h>

#define EFD_SEMAPHORE 1
#define EFD_CLOEXEC   O_CLOEXEC
#define EFD_NONBLOCK  O_NONBLOCK

#define EFD_MAX 0xfffffffffffffffeULL /* what the counter can hold */

/* a stream: no inode lock, `lock` covers the counter */
struct eventfd {
    spinlock_t        lock; /* before the wait_queue lock: it is released before every wake_up */
    struct wait_queue wq;   /* readers and writers both */
    uint64_t          count;
    bool              semaphore;
};

static const struct inode_ops eventfd_ops;

static struct eventfd *eventfd_of(struct inode *inode) {
    return inode->priv;
}

static long eventfd_read(struct file *f, void *buf, size_t n) {
    struct eventfd *e = eventfd_of(f->inode);

    if (n < sizeof(uint64_t)) return -EINVAL;

    uint64_t flags = spin_lock_irqsave(&e->lock);

    while (e->count == 0) {
        spin_unlock_irqrestore(&e->lock, flags);

        if (f->flags & O_NONBLOCK) return -EAGAIN;

        int err = wait_event_interruptible(e->wq, e->count);
        if (err) return err;

        flags = spin_lock_irqsave(&e->lock);
    }

    uint64_t value = e->semaphore ? 1 : e->count;
    e->count -= value;

    spin_unlock_irqrestore(&e->lock, flags);

    wake_up(&e->wq);

    memcpy(buf, &value, sizeof(value));
    return sizeof(value);
}

static long eventfd_write(struct file *f, const void *buf, size_t n) {
    struct eventfd *e = eventfd_of(f->inode);
    uint64_t value;

    if (n < sizeof(value)) return -EINVAL;

    memcpy(&value, buf, sizeof(value));
    if (value == ~0ULL) return -EINVAL;

    uint64_t flags = spin_lock_irqsave(&e->lock);

    while (e->count > EFD_MAX - value) {
        spin_unlock_irqrestore(&e->lock, flags);

        if (f->flags & O_NONBLOCK) return -EAGAIN;

        int err = wait_event_interruptible(e->wq, e->count <= EFD_MAX - value);
        if (err) return err;

        flags = spin_lock_irqsave(&e->lock);
    }

    e->count += value;

    spin_unlock_irqrestore(&e->lock, flags);

    wake_up(&e->wq);
    return sizeof(value);
}

static uint32_t eventfd_poll(struct inode *inode, uint32_t fflags, struct poll_table *pt) {
    struct eventfd *e = eventfd_of(inode);
    uint32_t mask = 0;

    (void)fflags;

    poll_wait(pt, &e->wq);

    uint64_t flags = spin_lock_irqsave(&e->lock);

    if (e->count)            mask |= POLLIN;
    if (e->count < EFD_MAX)  mask |= POLLOUT;

    spin_unlock_irqrestore(&e->lock, flags);
    return mask;
}

static void eventfd_evict(struct inode *inode) {
    kfree(inode->priv);
}

static const struct inode_ops eventfd_ops = {
    .stream       = true,
    .stream_read  = eventfd_read,
    .stream_write = eventfd_write,
    .poll         = eventfd_poll,
    .evict        = eventfd_evict,
};

long sys_eventfd2(long initval, long flags) {
    if (flags & ~(EFD_SEMAPHORE | EFD_CLOEXEC | EFD_NONBLOCK)) return -EINVAL;

    struct eventfd *e = kmalloc(sizeof(*e));
    if (!e) return -ENOMEM;

    spin_lock_init(&e->lock);
    wait_queue_init(&e->wq);

    e->count     = (uint32_t)initval;
    e->semaphore = flags & EFD_SEMAPHORE;

    struct inode *inode = inode_alloc(&eventfd_ops, 0600);
    if (!inode) {
        kfree(e);
        return -ENOMEM;
    }

    inode->priv = e; /* evict frees it, from here on */

    return anon_fd(inode, O_RDWR | (flags & EFD_NONBLOCK), flags & EFD_CLOEXEC);
}
