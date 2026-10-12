#include <serika/cpu.h>
#include <serika/errno.h>
#include <serika/fcntl.h>
#include <serika/mm.h>
#include <serika/poll.h>
#include <serika/preempt.h>
#include <serika/sched.h>
#include <serika/signal.h>
#include <serika/slab.h>
#include <serika/string.h>
#include <serika/syscall.h>
#include <serika/task.h>
#include <serika/timer.h>
#include <serika/vfs.h>
#include <serika/wait.h>

#include <asm/arch.h>

#define TFD_CLOEXEC  O_CLOEXEC
#define TFD_NONBLOCK O_NONBLOCK

#define TFD_TIMER_ABSTIME       1
#define TFD_TIMER_CANCEL_ON_SET 2

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1
#define CLOCK_BOOTTIME  7

#define NSEC_PER_SEC 1000000000ULL

struct timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

struct itimerspec {
    struct timespec it_interval;
    struct timespec it_value;
};

/* a stream: no inode lock, `lock` covers all of this. the expirations are counted when somebody looks, so the
 * timer only has to wake the sleepers: its fn takes no lock of ours */
struct timerfd {
    spinlock_t        lock; /* before timer_lock, and before the wait_queue lock */
    struct wait_queue wq;
    struct timer      timer;
    long              clock;
    uint64_t          expires;  /* tsc, of the next one. 0: not armed */
    uint64_t          interval; /* ticks. 0: once */
    uint64_t          ticks;    /* expired, not read yet */
};

static const struct inode_ops timerfd_ops;

static struct timerfd *timerfd_of(struct inode *inode) {
    return inode->priv;
}

static void timerfd_fire(struct timer *timer) {
    wake_up(&container_of(timer, struct timerfd, timer)->wq);
}

/* lock held. the timer goes where expires is */
static void timerfd_arm(struct timerfd *t) {
    timer_del(&t->timer);

    if (!t->expires) return;

    t->timer.expires = t->expires;
    timer_add(&t->timer);
}

/* lock held. what has expired since the last look is counted, and the next one is waited for */
static void timerfd_update(struct timerfd *t) {
    uint64_t now = rdtsc();

    if (!t->expires || now < t->expires) return;

    if (t->interval) {
        uint64_t n = (now - t->expires) / t->interval + 1;

        t->ticks += n;
        t->expires += n * t->interval;
    } else {
        t->ticks++;
        t->expires = 0;
    }

    timerfd_arm(t);
}

static long timerfd_read(struct file *f, void *buf, size_t n) {
    struct timerfd *t = timerfd_of(f->inode);

    if (n < sizeof(uint64_t)) return -EINVAL;

    uint64_t flags = spin_lock_irqsave(&t->lock);

    for (;;) {
        timerfd_update(t);
        if (t->ticks) break;

        spin_unlock_irqrestore(&t->lock, flags);

        if (f->flags & O_NONBLOCK) return -EAGAIN;

        /* the timer wakes us. the look is the condition: it is when the count moves */
        int err = wait_event_interruptible(t->wq, ({
            uint64_t __f = spin_lock_irqsave(&t->lock);
            timerfd_update(t);
            bool __ready = t->ticks != 0;
            spin_unlock_irqrestore(&t->lock, __f);
            __ready;
        }));
        if (err) return err;

        flags = spin_lock_irqsave(&t->lock);
    }

    uint64_t ticks = t->ticks;
    t->ticks = 0;

    spin_unlock_irqrestore(&t->lock, flags);

    memcpy(buf, &ticks, sizeof(ticks));
    return sizeof(ticks);
}

static uint32_t timerfd_poll(struct inode *inode, uint32_t fflags, struct poll_table *pt) {
    struct timerfd *t = timerfd_of(inode);

    (void)fflags;

    poll_wait(pt, &t->wq);

    uint64_t flags = spin_lock_irqsave(&t->lock);

    timerfd_update(t);
    uint32_t mask = t->ticks ? POLLIN : 0;

    spin_unlock_irqrestore(&t->lock, flags);
    return mask;
}

static void timerfd_evict(struct inode *inode) {
    struct timerfd *t = inode->priv;

    timer_del(&t->timer); /* the tick does not have t after this */
    kfree(t);
}

static const struct inode_ops timerfd_ops = {
    .stream      = true,
    .stream_read = timerfd_read,
    .poll        = timerfd_poll,
    .evict       = timerfd_evict,
};

long sys_timerfd_create(long clock, long flags) {
    if (flags & ~(TFD_CLOEXEC | TFD_NONBLOCK)) return -EINVAL;
    if (clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC && clock != CLOCK_BOOTTIME) return -EINVAL;

    struct timerfd *t = kmalloc(sizeof(*t));
    if (!t) return -ENOMEM;

    memset(t, 0, sizeof(*t));
    spin_lock_init(&t->lock);
    wait_queue_init(&t->wq);

    t->clock    = clock;
    t->timer.fn = timerfd_fire;

    struct inode *inode = inode_alloc(&timerfd_ops, 0600);
    if (!inode) {
        kfree(t);
        return -ENOMEM;
    }

    inode->priv = t; /* evict frees it, from here on */

    return anon_fd(inode, O_RDWR | (flags & TFD_NONBLOCK), flags & TFD_CLOEXEC);
}

/* ticks as a timespec */
static struct timespec ticks_to_timespec(uint64_t ticks) {
    uint64_t freq = this_cpu_read(tsc_freq_hz);

    return (struct timespec){ .tv_sec = ticks / freq, .tv_nsec = (ticks % freq) * NSEC_PER_SEC / freq };
}

/* lock held. what is left to the next expiry, and the interval */
static struct itimerspec timerfd_left(struct timerfd *t) {
    uint64_t now = rdtsc();

    timerfd_update(t);

    return (struct itimerspec){
        .it_interval = ticks_to_timespec(t->interval),
        .it_value    = ticks_to_timespec(t->expires > now ? t->expires - now : 0),
    };
}

/* the file is referenced: vfs_close() it */
static int timerfd_get(long fd, struct file **out) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;

    if (f->inode->ops != &timerfd_ops) {
        vfs_close(f);
        return -EINVAL;
    }

    *out = f;
    return 0;
}

long sys_timerfd_settime(long fd, long flags, long unew, long uold) {
    struct file *f;
    uint64_t interval, value;

    if (flags & ~(TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET)) return -EINVAL;

    /* an itimerspec is two timespecs: the interval, then the value */
    long err = timespec_to_tsc(unew, &interval);
    if (err) return err;

    if ((err = timespec_to_tsc(unew + sizeof(struct timespec), &value)) != 0) return err;

    if ((err = timerfd_get(fd, &f)) != 0) return err;

    struct timerfd *t = timerfd_of(f->inode);
    uint64_t when = 0;

    if (value) {
        uint64_t now;

        clock_tsc(t->clock, &now);

        if (flags & TFD_TIMER_ABSTIME) value = value > now ? value - now : 0;

        when = rdtsc() + value;
    }

    uint64_t lflags = spin_lock_irqsave(&t->lock);

    struct itimerspec old = timerfd_left(t);

    t->ticks    = 0;
    t->interval = value ? interval : 0;
    t->expires  = when;
    timerfd_arm(t);

    spin_unlock_irqrestore(&t->lock, lflags);

    wake_up(&t->wq); /* one asleep on the old time looks again */
    vfs_close(f);

    if (uold && copy_to_user(uold, &old, sizeof(old)) != 0) return -EFAULT;
    return 0;
}

long sys_timerfd_gettime(long fd, long ucur) {
    struct file *f;

    long err = timerfd_get(fd, &f);
    if (err) return err;

    struct timerfd *t = timerfd_of(f->inode);
    uint64_t flags = spin_lock_irqsave(&t->lock);
    struct itimerspec cur = timerfd_left(t);
    spin_unlock_irqrestore(&t->lock, flags);

    vfs_close(f);

    return copy_to_user(ucur, &cur, sizeof(cur));
}
