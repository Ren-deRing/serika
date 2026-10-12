#include <serika/errno.h>
#include <serika/fcntl.h>
#include <serika/mm.h>
#include <serika/panic.h>
#include <serika/poll.h>
#include <serika/printk.h>
#include <serika/preempt.h>
#include <serika/sched.h>
#include <serika/signal.h>
#include <serika/slab.h>
#include <serika/stat.h>
#include <serika/string.h>
#include <serika/syscall.h>
#include <serika/task.h>
#include <serika/tty.h>
#include <serika/vfs.h>
#include <serika/wait.h>

/* a pseudo terminal. an open of /dev/ptmx is the master of a new one, and its slave is a tty at /dev/pts/<n>: what the
 * slave's program writes the master reads, and what the master writes is what the slave's line discipline receives, as
 * if typed */

#define PTMX_RDEV   (5 << 8 | 2)
#define PTS_MAJOR   136
#define PTY_MAX     256
#define PTY_BUF     4096

#define TIOCGPTN    0x80045430
#define TIOCSPTLCK  0x40045431
#define TIOCGPTLCK  0x80045439

struct pty {
    struct tty_struct *tty; /* the slave */
    int                index;

    spinlock_t         lock; /* the ring and what follows. a leaf: the slave's echo writes with the tty's lock held */
    struct wait_queue  wq;   /* the master: readable, or the slave went */
    uint8_t           *buf; /* PTY_BUF: a page is the most kmalloc() gives, and the rest of this goes in it too */
    uint32_t           head, tail; /* counters, as n_tty's */
    uint32_t           slave_opens;
    bool               slave_seen;  /* opened once: before that the slave has not hung up, it has not come yet */
    bool               master_closed;
    bool               locked;      /* unlockpt() has not been called */
};

static spinlock_t pty_lock = SPINLOCK_INITIALIZER; /* the numbers */
static bool       pty_used[PTY_MAX];

static int index_alloc(void) {
    int index = -1;
    uint64_t flags = spin_lock_irqsave(&pty_lock);

    for (int i = 0; i < PTY_MAX; i++) {
        if (!pty_used[i]) {
            pty_used[i] = true;
            index = i;
            break;
        }
    }

    spin_unlock_irqrestore(&pty_lock, flags);
    return index;
}

static void index_free(int index) {
    uint64_t flags = spin_lock_irqsave(&pty_lock);
    pty_used[index] = false;
    spin_unlock_irqrestore(&pty_lock, flags);
}

static bool ring_full(struct pty *p, uint32_t need) {
    return PTY_BUF - (p->head - p->tail) < need;
}

static bool slave_gone(struct pty *p) {
    return p->slave_seen && !p->slave_opens;
}

/* ---- the slave ---- */

/* what the slave's program writes, and the echo. output processing is here: onlcr is what a terminal wants. the
 * tty's lock may be held, so nothing sleeps */
static long pty_write(struct tty_struct *tty, const char *buf, size_t n) {
    struct pty *p = tty->driver_data;
    bool onlcr = (tty->termios.c_oflag & (OPOST | ONLCR)) == (OPOST | ONLCR);
    size_t i = 0;

    uint64_t flags = spin_lock_irqsave(&p->lock);

    if (p->master_closed) {
        spin_unlock_irqrestore(&p->lock, flags);
        return n; /* nobody is there to read it */
    }

    for (; i < n; i++) {
        uint32_t need = buf[i] == '\n' && onlcr ? 2 : 1;

        if (ring_full(p, need)) break;

        if (need == 2) p->buf[p->head++ % PTY_BUF] = '\r';
        p->buf[p->head++ % PTY_BUF] = buf[i];
    }

    spin_unlock_irqrestore(&p->lock, flags);

    if (i) wake_up(&p->wq);
    return i;
}

static bool pty_write_room(struct tty_struct *tty) {
    struct pty *p = tty->driver_data;

    return p->master_closed || !ring_full(p, 2);
}

static int pty_open(struct tty_struct *tty) {
    struct pty *p = tty->driver_data;

    if (p->locked) return -EIO;

    uint64_t flags = spin_lock_irqsave(&p->lock);
    p->slave_opens++;
    p->slave_seen = true;
    spin_unlock_irqrestore(&p->lock, flags);

    return 0;
}

static void pty_close(struct tty_struct *tty) {
    struct pty *p = tty->driver_data;

    uint64_t flags = spin_lock_irqsave(&p->lock);
    p->slave_opens--;
    spin_unlock_irqrestore(&p->lock, flags);

    wake_up(&p->wq); /* the master hears of the last one */
}

/* the slave's inode is gone: the master and every file of the slave are */
static void pty_free(struct tty_struct *tty) {
    struct pty *p = tty->driver_data;

    index_free(p->index);
    kfree(p->buf);
    kfree(p);
}

static const struct tty_operations pty_ops = {
    .write      = pty_write,
    .write_room = pty_write_room,
    .open       = pty_open,
    .close      = pty_close,
    .free       = pty_free,
};

/* ---- the master: f->priv is the pty ---- */

static void slave_path(struct pty *p, char *path) {
    ksnprintf(path, 24, "/dev/pts/%d", p->index);
}

static int ptmx_open(struct file *f) {
    struct pty *p = kmalloc(sizeof(*p));
    if (!p) return -ENOMEM;

    memset(p, 0, sizeof(*p));

    p->buf = kmalloc(PTY_BUF);
    if (!p->buf) {
        kfree(p);
        return -ENOMEM;
    }

    p->index = index_alloc();
    if (p->index < 0) {
        kfree(p->buf);
        kfree(p);
        return -ENOSPC;
    }

    spin_lock_init(&p->lock);
    wait_queue_init(&p->wq);
    p->locked = true;

    p->tty = tty_alloc(&pty_ops, p);
    if (!p->tty) {
        index_free(p->index);
        kfree(p->buf);
        kfree(p);
        return -ENOMEM;
    }

    struct inode *slave = tty_inode(p->tty); /* the reference tty_alloc() gave is the master's */

    slave->rdev = PTS_MAJOR << 8 | p->index;

    char path[24];

    slave_path(p, path);

    int err = vfs_link_inode(path, slave);
    if (err) {
        iput(slave); /* frees the pty with it */
        return err;
    }

    f->priv = p;
    return 0;
}

static void ptmx_release(struct file *f) {
    struct pty *p = f->priv;
    struct tty_struct *tty = p->tty;

    uint64_t flags = spin_lock_irqsave(&p->lock);
    p->master_closed = true;
    spin_unlock_irqrestore(&p->lock, flags);

    tty_ctty_drop(tty); /* before the master's reference to the slave goes: a /dev/tty open takes one under the list's lock */

    flags = spin_lock_irqsave(&tty->lock);
    tty->hung = true;
    long pgrp = tty->pgrp;
    spin_unlock_irqrestore(&tty->lock, flags);

    wake_up(&tty->read_wait);
    wake_up(&tty->write_wait);

    if (pgrp) {
        struct ksiginfo info = { .signo = SIGHUP, .code = SI_KERNEL };

        task_signal_pgrp(pgrp, SIGHUP, &info);
    }

    char path[24];

    slave_path(p, path);
    vfs_unlink(path);

    iput(tty->inode); /* the last of the slave's files frees it all */
}

static long ptmx_read(struct file *f, void *buf, size_t n) {
    struct pty *p = f->priv;

    if (!n) return 0;

    for (;;) {
        uint64_t flags = spin_lock_irqsave(&p->lock);

        size_t avail = p->head - p->tail;

        if (avail) {
            size_t len = avail < n ? avail : n;

            for (size_t i = 0; i < len; i++) ((uint8_t *)buf)[i] = p->buf[p->tail++ % PTY_BUF];

            spin_unlock_irqrestore(&p->lock, flags);

            wake_up(&p->tty->write_wait);
            return len;
        }

        bool gone = slave_gone(p);

        spin_unlock_irqrestore(&p->lock, flags);

        if (gone) return -EIO;
        if (f->flags & O_NONBLOCK) return -EAGAIN;

        int err = wait_event_interruptible(p->wq, p->head != p->tail || slave_gone(p));
        if (err) return err;
    }
}

/* typed at the slave */
static long ptmx_write(struct file *f, const void *buf, size_t n) {
    struct pty *p = f->priv;

    p->tty->ldisc->receive_buf(p->tty, buf, n);
    return n;
}

static long ptmx_ioctl(struct file *f, unsigned long cmd, unsigned long arg) {
    struct pty *p = f->priv;

    switch (cmd) {
        case TIOCGPTN: {
            int32_t index = p->index;

            return copy_to_user(arg, &index, sizeof(index));
        }

        case TIOCSPTLCK: {
            int32_t lock;
            int err = copy_from_user(&lock, arg, sizeof(lock));

            if (err) return err;

            p->locked = lock != 0;
            return 0;
        }

        case TIOCGPTLCK: {
            int32_t lock = p->locked;

            return copy_to_user(arg, &lock, sizeof(lock));
        }
    }

    /* the rest is the terminal's: termios and the window size are the slave's */
    return tty_ioctl(p->tty->inode, cmd, arg);
}

static uint32_t ptmx_poll(struct file *f, struct poll_table *pt) {
    struct pty *p = f->priv;

    poll_wait(pt, &p->wq);

    uint64_t flags = spin_lock_irqsave(&p->lock);
    uint32_t mask = POLLOUT | (p->head != p->tail ? POLLIN : 0) | (slave_gone(p) ? POLLHUP : 0);
    spin_unlock_irqrestore(&p->lock, flags);

    return mask;
}

static const struct inode_ops ptmx_ops = {
    .stream       = true,
    .stream_read  = ptmx_read,
    .stream_write = ptmx_write,
    .open_file    = ptmx_open,
    .release_file = ptmx_release,
    .ioctl_file   = ptmx_ioctl,
    .poll_file    = ptmx_poll,
};

void pty_devices_create(void) {
    if (vfs_mkdir("/dev/pts", 0755) != 0) panic("pty: unable to create /dev/pts -- out of memory?");

    struct inode *inode = inode_alloc(&ptmx_ops, S_IFCHR | 0666);
    if (!inode) panic("pty: unable to create /dev/ptmx -- out of memory?");

    inode->rdev = PTMX_RDEV;

    int err = vfs_link_inode("/dev/ptmx", inode);
    if (err) panic("pty: unable to create /dev/ptmx -- error %d", err);

    iput(inode);
}
