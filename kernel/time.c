#include <serika/cpu.h>
#include <serika/errno.h>
#include <serika/list.h>
#include <serika/lock.h>
#include <serika/mm.h>
#include <serika/preempt.h>
#include <serika/sched.h>
#include <serika/signal.h>
#include <serika/syscall.h>
#include <serika/timer.h>
#include <serika/wait.h>

#include <asm/arch.h>

#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3
#define CLOCK_MONOTONIC_RAW      4
#define CLOCK_REALTIME_COARSE    5
#define CLOCK_MONOTONIC_COARSE   6
#define CLOCK_BOOTTIME           7

#define TIMER_ABSTIME 1

#define NSEC_PER_SEC 1000000000ULL

struct timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

/* realtime is the tsc plus this, the epoch at boot in ticks */
static uint64_t realtime_offset;

void time_init(void) {
    realtime_offset = rtc_read_epoch() * this_cpu_read(tsc_freq_hz);
}

/* a point on CLOCK_REALTIME in ticks, as a point on the tsc */
uint64_t realtime_to_tsc(uint64_t t) {
    return t > realtime_offset ? t - realtime_offset : 1; /* 0 means no deadline */
}

uint64_t realtime_ns(void) {
    uint64_t freq = this_cpu_read(tsc_freq_hz), tsc = rdtsc() + realtime_offset;

    return tsc / freq * NSEC_PER_SEC + tsc % freq * NSEC_PER_SEC / freq;
}

int clock_tsc(long clk, uint64_t *tsc) {
    switch (clk) {
        case CLOCK_REALTIME:
        case CLOCK_REALTIME_COARSE:
            *tsc = rdtsc() + realtime_offset;
            return 0;

        case CLOCK_MONOTONIC:
        case CLOCK_MONOTONIC_RAW:
        case CLOCK_MONOTONIC_COARSE:
        case CLOCK_BOOTTIME:
            *tsc = rdtsc();
            return 0;

        case CLOCK_PROCESS_CPUTIME_ID: /* no thread groups: a process is a task */
        case CLOCK_THREAD_CPUTIME_ID:
            *tsc = current_task()->sum_exec;
            return 0;
    }

    return -EINVAL;
}

long sys_clock_gettime(long clk, long utp) {
    uint64_t tsc, freq = this_cpu_read(tsc_freq_hz);

    int err = clock_tsc(clk, &tsc);
    if (err) return err;

    struct timespec ts = {
        .tv_sec  = tsc / freq,
        .tv_nsec = (tsc % freq) * NSEC_PER_SEC / freq,
    };

    return copy_to_user(utp, &ts, sizeof(ts));
}

struct timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

/* the zone is not kept, as in linux */
long sys_gettimeofday(long utv, long utz) {
    (void)utz;

    if (!utv) return 0;

    uint64_t ns = realtime_ns();
    struct timeval tv = { .tv_sec = ns / NSEC_PER_SEC, .tv_usec = ns % NSEC_PER_SEC / 1000 };

    return copy_to_user(utv, &tv, sizeof(tv));
}

long sys_time(long utloc) {
    int64_t sec = realtime_ns() / NSEC_PER_SEC;

    if (utloc && copy_to_user(utloc, &sec, sizeof(sec)) != 0) return -EFAULT;
    return sec;
}

/* sorted by expiry. timer_lock, then a wait_queue lock, then an rq lock: fn wakes someone */
static spinlock_t timer_lock = SPINLOCK_INITIALIZER;
static list_node  timers = LIST_HEAD_INIT(timers);

void timer_add(struct timer *t) {
    uint64_t flags = spin_lock_irqsave(&timer_lock);

    list_node *at = &timers;
    while (at->next != &timers && container_of(at->next, struct timer, node)->expires <= t->expires) at = at->next;

    __list_add(&t->node, at, at->next);
    t->queued = true;

    spin_unlock_irqrestore(&timer_lock, flags);
}

bool timer_del(struct timer *t) {
    uint64_t flags = spin_lock_irqsave(&timer_lock);

    bool pending = t->queued;
    if (pending) list_del(&t->node);
    t->queued = false;

    spin_unlock_irqrestore(&timer_lock, flags);
    return pending;
}

/* in the tick: irq off, one cpu only so the list is not fought over */
void timer_tick(void) {
    if (list_empty(&timers)) return; /* a hint. the next tick looks again */

    uint64_t now = rdtsc();

    spin_lock(&timer_lock);

    while (!list_empty(&timers)) {
        struct timer *t = list_first(&timers, struct timer, node);
        if (t->expires > now) break;

        list_del(&t->node);
        t->queued = false;
        t->fn(t);
    }

    spin_unlock(&timer_lock);
}

struct sleeper {
    struct timer      timer;
    struct wait_queue wq;
    volatile bool     done;
};

static void sleeper_fire(struct timer *t) {
    struct sleeper *s = container_of(t, struct sleeper, timer);

    s->done = true;
    wake_up(&s->wq);
}

long sleep_until(uint64_t deadline) {
    struct sleeper s = { .timer = { .expires = deadline, .fn = sleeper_fire } };

    if (rdtsc() >= deadline) return 0;

    wait_queue_init(&s.wq);
    timer_add(&s.timer);

    int err = wait_event_interruptible(s.wq, s.done);

    /* after this the tick no longer has s */
    timer_del(&s.timer);

    return err ? -EINTR : 0;
}

/* what is left of it, for rem */
static int put_left(long urem, uint64_t deadline) {
    uint64_t now = rdtsc(), freq = this_cpu_read(tsc_freq_hz);
    uint64_t left = deadline > now ? deadline - now : 0;

    struct timespec ts = {
        .tv_sec  = left / freq,
        .tv_nsec = (left % freq) * NSEC_PER_SEC / freq,
    };

    return copy_to_user(urem, &ts, sizeof(ts));
}

int timespec_to_tsc(long uts, uint64_t *tsc) {
    struct timespec ts;
    uint64_t freq = this_cpu_read(tsc_freq_hz);

    int err = copy_from_user(&ts, uts, sizeof(ts));
    if (err) return err;

    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || (uint64_t)ts.tv_nsec >= NSEC_PER_SEC) return -EINVAL;

    /* as many seconds as half the counter holds, so a sum with the tsc cannot wrap. that is past the epoch of
     * an absolute CLOCK_REALTIME time, which is why it is not a year */
    uint64_t max_sec = (~0ULL >> 1) / freq;
    if ((uint64_t)ts.tv_sec > max_sec) ts.tv_sec = max_sec;

    *tsc = ts.tv_sec * freq + ts.tv_nsec * freq / NSEC_PER_SEC;
    return 0;
}

long sys_nanosleep(long ureq, long urem) {
    uint64_t delta;

    int err = timespec_to_tsc(ureq, &delta);
    if (err) return err;

    uint64_t deadline = rdtsc() + delta;

    if (sleep_until(deadline) == 0) return 0;

    if (urem && put_left(urem, deadline) != 0) return -EFAULT;
    return -EINTR;
}

long sys_clock_nanosleep(long clk, long flags, long ureq, long urem) {
    uint64_t when, now;

    if (clk == CLOCK_THREAD_CPUTIME_ID) return -EINVAL;

    int err = clock_tsc(clk, &now);
    if (err) return err;

    if ((err = timespec_to_tsc(ureq, &when)) != 0) return err;

    if (!(flags & TIMER_ABSTIME)) when += now;

    uint64_t deadline = rdtsc() + (when > now ? when - now : 0);

    if (sleep_until(deadline) == 0) return 0;

    if (urem && !(flags & TIMER_ABSTIME) && put_left(urem, deadline) != 0) return -EFAULT;
    return -EINTR;
}
