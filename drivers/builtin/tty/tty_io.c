#include <serika/errno.h>
#include <serika/fcntl.h>
#include <serika/mm.h>
#include <serika/panic.h>
#include <serika/poll.h>
#include <serika/preempt.h>
#include <serika/sched.h>
#include <serika/signal.h>
#include <serika/slab.h>
#include <serika/stat.h>
#include <serika/string.h>
#include <serika/syscall.h>
#include <serika/task.h>
#include <serika/tty.h>
#include <serika/wait.h>
#include <serika/vfs.h>

#define TCGETS      0x5401
#define TCSETS      0x5402
#define TCSETSW     0x5403
#define TCSETSF     0x5404
#define TCFLSH      0x540B
#define TIOCSCTTY   0x540E
#define TIOCGPGRP   0x540F
#define TIOCSPGRP   0x5410
#define TIOCGWINSZ  0x5413
#define TIOCSWINSZ  0x5414
#define FIONREAD    0x541B
#define TIOCNOTTY   0x5422
#define TIOCGSID    0x5429
#define TCGETS2     0x802c542a
#define TCSETS2     0x402c542b
#define TCSETSW2    0x402c542c
#define TCSETSF2    0x402c542d

#define TCIFLUSH  0
#define TCIOFLUSH 2

struct tty_struct *console_tty;

/* controlling terminals. a session has the one tty it opened first as its leader, and /dev/tty opens that. the list is
 * the ttys that are one, and an entry is there while the pty's master is: it holds the slave's inode */
static spinlock_t        ctty_lock = SPINLOCK_INITIALIZER;
static struct tty_struct *ctty_list;

/* the caller's session: 0 for a kernel thread, which has none */
static long caller_sid(void) {
    return current_task()->group ? sys_getsid(0) : 0;
}

/* the session of the caller is its leader's, so the tty becomes its own, and its foreground group is the leader's.
 * the console is every session's already */
static void ctty_acquire(struct tty_struct *tty) {
    long sid = caller_sid();

    if (tty == console_tty || !sid || sid != sys_getpid()) return;

    uint64_t flags = spin_lock_irqsave(&ctty_lock);

    bool taken = tty->sid != 0;

    for (struct tty_struct *t = ctty_list; t && !taken; t = t->ctty_next)
        if (t->sid == sid) taken = true; /* one a session */

    if (!taken) {
        tty->sid = sid;
        tty->ctty_next = ctty_list;
        ctty_list = tty;
    }

    spin_unlock_irqrestore(&ctty_lock, flags);

    if (taken) return;

    flags = spin_lock_irqsave(&tty->lock);
    tty->pgrp = sys_getpgrp();
    spin_unlock_irqrestore(&tty->lock, flags);
}

void tty_ctty_drop(struct tty_struct *tty) {
    uint64_t flags = spin_lock_irqsave(&ctty_lock);

    for (struct tty_struct **p = &ctty_list; *p; p = &(*p)->ctty_next) {
        if (*p == tty) {
            *p = tty->ctty_next;
            break;
        }
    }

    tty->sid = 0;

    spin_unlock_irqrestore(&ctty_lock, flags);
}

void tty_insert_flip_char(struct tty_struct *tty, uint8_t c) {
    if (tty->flip_len == TTY_FLIP_SIZE) tty_flip_buffer_push(tty);

    tty->flip[tty->flip_len++] = c;
}

void tty_flip_buffer_push(struct tty_struct *tty) {
    if (!tty->flip_len) return;

    tty->ldisc->receive_buf(tty, tty->flip, tty->flip_len);
    tty->flip_len = 0;
}

/* a stream: read sleeps, so the inode lock is not held */
static long tty_read(struct file *f, void *buf, size_t n) {
    struct tty_struct *tty = f->inode->priv;
    return tty->ldisc->read(tty, buf, n, f->flags & O_NONBLOCK);
}

static long tty_write(struct file *f, const void *buf, size_t n) {
    struct tty_struct *tty = f->inode->priv;
    size_t done = 0;

    while (done < n) {
        long w = tty->ops->write(tty, (const char *)buf + done, n - done);

        if (w > 0) {
            done += w;
            continue;
        }

        if (f->flags & O_NONBLOCK) return done ? (long)done : -EAGAIN;

        int err = wait_event_interruptible(tty->write_wait, tty->ops->write_room(tty));
        if (err) return done ? (long)done : err;
    }

    return done;
}

/* output never waits: the uart is polled */
static uint32_t tty_poll(struct inode *inode, uint32_t fflags, struct poll_table *pt) {
    (void)fflags;

    struct tty_struct *tty = inode->priv;

    poll_wait(pt, &tty->read_wait);

    uint64_t flags = spin_lock_irqsave(&tty->lock);
    uint32_t mask = POLLOUT | (tty->ldisc->readable(tty) ? POLLIN : 0) | (tty->hung ? POLLHUP : 0);
    spin_unlock_irqrestore(&tty->lock, flags);

    return mask;
}

static void set_termios(struct tty_struct *tty, const struct ktermios *new, bool flush) {
    uint64_t flags = spin_lock_irqsave(&tty->lock);

    struct ktermios old = tty->termios;
    tty->termios = *new;

    tty->ldisc->set_termios(tty, &old);
    if (flush) tty->ldisc->flush_buffer(tty);

    spin_unlock_irqrestore(&tty->lock, flags);

    wake_up(&tty->read_wait); /* the mode may have made something ready */
}

long tty_ioctl(struct inode *inode, unsigned long cmd, unsigned long arg) {
    struct tty_struct *tty = inode->priv;
    uint64_t flags;
    int err;

    switch (cmd) {
        case TCGETS:
        case TCGETS2: {
            flags = spin_lock_irqsave(&tty->lock);
            struct ktermios t = tty->termios;
            spin_unlock_irqrestore(&tty->lock, flags);

            return copy_to_user(arg, &t, cmd == TCGETS ? TTY_TERMIOS_SIZE : sizeof(t));
        }

        case TCSETS:
        case TCSETSW:
        case TCSETSF:
        case TCSETS2:
        case TCSETSW2:
        case TCSETSF2: {
            bool two = cmd == TCSETS2 || cmd == TCSETSW2 || cmd == TCSETSF2;
            struct ktermios t;

            /* termios has no speeds: they stay */
            flags = spin_lock_irqsave(&tty->lock);
            t = tty->termios;
            spin_unlock_irqrestore(&tty->lock, flags);

            err = copy_from_user(&t, arg, two ? sizeof(t) : TTY_TERMIOS_SIZE);
            if (err) return err;

            set_termios(tty, &t, cmd == TCSETSF || cmd == TCSETSF2);
            return 0;
        }

        case TCFLSH:
            if (arg > TCIOFLUSH) return -EINVAL;

            if (arg == TCIFLUSH || arg == TCIOFLUSH) {
                flags = spin_lock_irqsave(&tty->lock);
                tty->ldisc->flush_buffer(tty);
                spin_unlock_irqrestore(&tty->lock, flags);
            }

            return 0;

        case FIONREAD: {
            flags = spin_lock_irqsave(&tty->lock);
            int32_t n = tty->ldisc->input_pending(tty);
            spin_unlock_irqrestore(&tty->lock, flags);

            return copy_to_user(arg, &n, sizeof(n));
        }

        /* the foreground group is whoever set it, else the asker's own */
        case TIOCGPGRP: {
            flags = spin_lock_irqsave(&tty->lock);
            int32_t pgrp = tty->pgrp ? tty->pgrp : sys_getpgrp();
            spin_unlock_irqrestore(&tty->lock, flags);

            return copy_to_user(arg, &pgrp, sizeof(pgrp));
        }

        case TIOCSPGRP: {
            int32_t pgrp;

            err = copy_from_user(&pgrp, arg, sizeof(pgrp));
            if (err) return err;
            if (pgrp <= 0) return -EINVAL;

            flags = spin_lock_irqsave(&tty->lock);
            tty->pgrp = pgrp;
            spin_unlock_irqrestore(&tty->lock, flags);
            return 0;
        }

        case TIOCGWINSZ: {
            flags = spin_lock_irqsave(&tty->lock);
            struct winsize ws = tty->winsize;
            spin_unlock_irqrestore(&tty->lock, flags);

            return copy_to_user(arg, &ws, sizeof(ws));
        }

        case TIOCSWINSZ: {
            struct winsize ws;

            err = copy_from_user(&ws, arg, sizeof(ws));
            if (err) return err;

            flags = spin_lock_irqsave(&tty->lock);
            bool changed = memcmp(&tty->winsize, &ws, sizeof(ws)) != 0;
            long pgrp = tty->pgrp;
            tty->winsize = ws;
            spin_unlock_irqrestore(&tty->lock, flags);

            if (changed && pgrp) {
                struct ksiginfo info = { .signo = SIGWINCH, .code = SI_KERNEL };

                task_signal_pgrp(pgrp, SIGWINCH, &info);
            }

            return 0;
        }

        /* the console is every process's: nothing to take or give up */
        case TIOCSCTTY:
            ctty_acquire(tty);
            return 0;

        case TIOCNOTTY:
            return 0;

        case TIOCGSID: {
            int32_t sid = sys_getsid(0);

            return copy_to_user(arg, &sid, sizeof(sid));
        }
    }

    return -ENOTTY;
}

static int tty_open(struct inode *inode, uint32_t flags) {
    struct tty_struct *tty = inode->priv;

    int err = tty->ops->open ? tty->ops->open(tty) : 0;

    /* opened by a session leader that has none: this is it, unless it says it does not want one */
    if (!err && !(flags & O_NOCTTY)) ctty_acquire(tty);

    return err;
}

static void tty_release(struct inode *inode, uint32_t flags) {
    struct tty_struct *tty = inode->priv;

    (void)flags;

    if (tty->ops->close) tty->ops->close(tty);
}

/* only a pty's goes: the console is there for good */
static void tty_evict(struct inode *inode) {
    struct tty_struct *tty = inode->priv;

    if (tty->ops->free) tty->ops->free(tty);

    kfree(tty->read_buf);
    kfree(tty);
}

static const struct inode_ops tty_inode_ops = {
    .stream_read  = tty_read,
    .stream_write = tty_write,
    .ioctl        = tty_ioctl,
    .poll         = tty_poll,
    .open         = tty_open,
    .release      = tty_release,
    .evict        = tty_evict,
    .stream       = true,
};

/* cooked, echo, 38400 8n1 */
struct tty_struct *tty_alloc(const struct tty_operations *ops, void *driver_data) {
    struct tty_struct *tty = kmalloc(sizeof(struct tty_struct));
    if (!tty) return NULL;

    memset(tty, 0, sizeof(*tty));

    tty->read_buf = kmalloc(N_TTY_BUF_SIZE);
    tty->inode = inode_alloc(&tty_inode_ops, S_IFCHR | 0620);

    if (!tty->read_buf || !tty->inode) {
        kfree(tty->read_buf);
        if (tty->inode) iput(tty->inode);
        kfree(tty);
        return NULL;
    }

    tty->inode->priv = tty;
    tty->inode->rdev = 0x501; /* 5:1, the console */
    tty->ops = ops;
    tty->ldisc = &n_tty_ops;
    tty->driver_data = driver_data;

    spin_lock_init(&tty->lock);
    wait_queue_init(&tty->read_wait);
    wait_queue_init(&tty->write_wait);

    tty->termios = (struct ktermios){
        .c_iflag = ICRNL,
        .c_oflag = OPOST | ONLCR,
        .c_cflag = 0x00bf, /* B38400 | CS8 | CREAD */
        .c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE | IEXTEN,
        .c_cc = {
            [VINTR] = 3, [VQUIT] = 28, [VERASE] = 127, [VKILL] = 21, [VEOF] = 4, [VMIN] = 1,
            [VSTART] = 17, [VSTOP] = 19, [VSUSP] = 26, [VREPRINT] = 18, [VDISCARD] = 15, [VWERASE] = 23,
            [VLNEXT] = 22,
        },
        .c_ispeed = 38400,
        .c_ospeed = 38400,
    };

    tty->winsize = (struct winsize){ .ws_row = 24, .ws_col = 80 };
    return tty;
}

struct inode *tty_inode(struct tty_struct *tty) {
    return tty->inode;
}

/* fd 0, 1 and 2 of the first task, and /dev/console. the caller's reference */
struct inode *console_create(void) {
    if (!console_tty) return NULL;

    iget(console_tty->inode);
    return console_tty->inode;
}

/* /dev/tty: the file is the caller's controlling terminal's. the console when there is none */
static int devtty_open_file(struct file *f) {
    struct tty_struct *tty = console_tty;
    long sid = caller_sid();

    uint64_t flags = spin_lock_irqsave(&ctty_lock);

    for (struct tty_struct *t = ctty_list; t; t = t->ctty_next) {
        if (t->sid == sid) {
            tty = t;
            break;
        }
    }

    iget(tty->inode); /* under the lock: the master, which is what takes it off the list, holds it */

    spin_unlock_irqrestore(&ctty_lock, flags);

    int err = tty_open(tty->inode, f->flags | O_NOCTTY);
    if (err) {
        iput(tty->inode);
        return err;
    }

    iput(f->inode);
    f->inode = tty->inode;
    return 0;
}

static const struct inode_ops devtty_ops = {
    .open_file = devtty_open_file,
};

void tty_dev_create(void) {
    struct inode *inode = inode_alloc(&devtty_ops, S_IFCHR | 0666);
    if (!inode) panic("tty: unable to create /dev/tty -- out of memory?");

    inode->rdev = 0x500;

    if (vfs_link_inode("/dev/tty", inode) != 0) panic("tty: unable to create /dev/tty -- out of memory?");

    iput(inode);
}
