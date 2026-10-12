#include <serika/buddy.h>
#include <serika/errno.h>
#include <serika/fcntl.h>
#include <serika/page.h>
#include <serika/poll.h>
#include <serika/preempt.h>
#include <serika/sched.h>
#include <serika/signal.h>
#include <serika/slab.h>
#include <serika/stat.h>
#include <serika/string.h>
#include <serika/syscall.h>
#include <serika/task.h>
#include <serika/vfs.h>
#include <serika/wait.h>

#define PIPE_ORDER 4 /* 64 KiB, linux's */
#define PIPE_SIZE  ORDER_TO_SIZE(PIPE_ORDER)
#define PIPE_BUF   4096 /* a write this small goes in whole or not at all */

/* a stream: no inode lock, `lock` covers all of this */
struct pipe {
    spinlock_t        lock; /* before a wait_queue lock: it is released before every wake_up */
    struct wait_queue readable;
    struct wait_queue writable;

    uint8_t          *buf;  /* mapped, PIPE_SIZE */
    uint32_t          head; /* next to read */
    uint32_t          count;
    uint32_t          readers; /* open files */
    uint32_t          writers;
};

static const struct inode_ops pipe_ops;

static struct pipe *pipe_of(struct inode *inode) {
    return inode->priv;
}

/* sleeping with the lock off: the wakers change the fields and wake_up() after */
static long pipe_read(struct file *f, void *buf, size_t n) {
    struct pipe *p = pipe_of(f->inode);
    uint64_t flags = spin_lock_irqsave(&p->lock);

    while (p->count == 0) {
        if (!p->writers) {
            spin_unlock_irqrestore(&p->lock, flags);
            return 0; /* the end */
        }

        spin_unlock_irqrestore(&p->lock, flags);

        if (f->flags & O_NONBLOCK) return -EAGAIN;

        int err = wait_event_interruptible(p->readable, p->count || !p->writers);
        if (err) return err;

        flags = spin_lock_irqsave(&p->lock);
    }

    if (n > p->count) n = p->count;

    size_t first = PIPE_SIZE - p->head;
    if (first > n) first = n;

    memcpy(buf, p->buf + p->head, first);
    memcpy((char *)buf + first, p->buf, n - first);

    p->head = (p->head + n) % PIPE_SIZE;
    p->count -= n;

    spin_unlock_irqrestore(&p->lock, flags);

    wake_up(&p->writable);
    return n;
}

static long pipe_write(struct file *f, const void *buf, size_t n) {
    struct pipe *p = pipe_of(f->inode);
    size_t done = 0;

    uint64_t flags = spin_lock_irqsave(&p->lock);

    while (done < n) {
        if (!p->readers) {
            spin_unlock_irqrestore(&p->lock, flags);

            send_sigpipe();

            return done ? (long)done : -EPIPE;
        }

        size_t want = n <= PIPE_BUF ? n : 1; /* the whole of a small write, else any room */

        if (PIPE_SIZE - p->count < want) {
            spin_unlock_irqrestore(&p->lock, flags);

            if (f->flags & O_NONBLOCK) return done ? (long)done : -EAGAIN;

            int err = wait_event_interruptible(p->writable, PIPE_SIZE - p->count >= want || !p->readers);
            if (err) return done ? (long)done : err;

            flags = spin_lock_irqsave(&p->lock);
            continue;
        }

        size_t chunk = n - done;
        if (chunk > PIPE_SIZE - p->count) chunk = PIPE_SIZE - p->count;

        size_t tail = (p->head + p->count) % PIPE_SIZE;
        size_t first = PIPE_SIZE - tail;
        if (first > chunk) first = chunk;

        memcpy(p->buf + tail, (const char *)buf + done, first);
        memcpy(p->buf, (const char *)buf + done + first, chunk - first);

        p->count += chunk;
        done += chunk;

        spin_unlock_irqrestore(&p->lock, flags);
        wake_up(&p->readable);
        flags = spin_lock_irqsave(&p->lock);
    }

    spin_unlock_irqrestore(&p->lock, flags);
    return done;
}

static uint32_t pipe_poll(struct inode *inode, uint32_t fflags, struct poll_table *pt) {
    struct pipe *p = pipe_of(inode);
    uint32_t mask = 0;

    bool writer = (fflags & O_ACCMODE) == O_WRONLY;
    poll_wait(pt, writer ? &p->writable : &p->readable);

    uint64_t flags = spin_lock_irqsave(&p->lock);

    if (writer) {
        if (!p->readers)                           mask |= POLLERR;
        else if (PIPE_SIZE - p->count >= PIPE_BUF) mask |= POLLOUT;
    } else {
        if (p->count)    mask |= POLLIN;
        if (!p->writers) mask |= POLLHUP;
    }

    spin_unlock_irqrestore(&p->lock, flags);
    return mask;
}

/* the last reader or writer going wakes the other side: the end, or EPIPE */
static void pipe_release(struct inode *inode, uint32_t fflags) {
    struct pipe *p = pipe_of(inode);

    uint64_t flags = spin_lock_irqsave(&p->lock);

    if ((fflags & O_ACCMODE) != O_WRONLY) p->readers--;
    if ((fflags & O_ACCMODE) != O_RDONLY) p->writers--;

    spin_unlock_irqrestore(&p->lock, flags);

    wake_up(&p->readable);
    wake_up(&p->writable);
}

/* a named pipe is opened: an end waits for the other one, unless O_NONBLOCK or both are ours */
static int pipe_open(struct inode *inode, uint32_t fflags) {
    struct pipe *p = pipe_of(inode);
    uint32_t acc = fflags & O_ACCMODE;
    bool nonblock = fflags & O_NONBLOCK;

    uint64_t flags = spin_lock_irqsave(&p->lock);

    if (acc == O_WRONLY && nonblock && !p->readers) {
        spin_unlock_irqrestore(&p->lock, flags);
        return -ENXIO;
    }

    if (acc != O_WRONLY) p->readers++;
    if (acc != O_RDONLY) p->writers++;

    spin_unlock_irqrestore(&p->lock, flags);

    wake_up(&p->readable); /* whoever waited for this end */
    wake_up(&p->writable);

    if (nonblock || acc == O_RDWR) return 0;

    int err = acc == O_RDONLY ? wait_event_interruptible(p->readable, p->writers)
                              : wait_event_interruptible(p->writable, p->readers);
    if (err) pipe_release(inode, fflags);

    return err;
}

static void pipe_evict(struct inode *inode) {
    struct pipe *p = pipe_of(inode);

    free_pages((void *)V2P((uintptr_t)p->buf), PIPE_ORDER);
    kfree(p);
}

static const struct inode_ops pipe_ops = {
    .stream_read  = pipe_read,
    .stream_write = pipe_write,
    .evict        = pipe_evict,
    .poll         = pipe_poll,
    .stream       = true,
    .open         = pipe_open,
    .release      = pipe_release,
};

/* a pipe with nobody on either end yet */
static struct inode *pipe_inode_new(uint32_t mode) {
    struct pipe *p = kmalloc(sizeof(struct pipe));
    if (!p) return NULL;

    memset(p, 0, sizeof(*p));
    spin_lock_init(&p->lock);
    wait_queue_init(&p->readable);
    wait_queue_init(&p->writable);

    void *phys = alloc_pages(PIPE_ORDER);
    struct inode *inode = phys ? inode_alloc(&pipe_ops, S_IFIFO | (mode & 07777)) : NULL;

    if (!inode) {
        if (phys) free_pages(phys, PIPE_ORDER);
        kfree(p);
        return NULL;
    }

    p->buf = (uint8_t *)P2V((uintptr_t)phys);
    inode->priv = p;

    return inode;
}

struct inode *fifo_create(uint32_t mode) {
    return pipe_inode_new(mode);
}

int pipe_create(struct file *out[2], uint32_t flags) {
    struct inode *inode = pipe_inode_new(0600);
    if (!inode) return -ENOMEM;

    pipe_of(inode)->readers = pipe_of(inode)->writers = 1;
    iget(inode); /* the second file's */

    struct file *r = file_alloc(inode, O_RDONLY | (flags & O_NONBLOCK));
    struct file *w = file_alloc(inode, O_WRONLY | (flags & O_NONBLOCK));

    if (!r || !w) {
        kfree(r);
        kfree(w);
        iput(inode);
        iput(inode);
        return -ENOMEM;
    }

    out[0] = r;
    out[1] = w;
    return 0;
}
