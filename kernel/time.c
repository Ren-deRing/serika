#include <serika/cpu.h>
#include <serika/errno.h>
#include <serika/mm.h>
#include <serika/sched.h>
#include <serika/syscall.h>

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

/* TODO: no rtc yet: realtime is that too */
static int clock_tsc(long clk, uint64_t *tsc) {
    switch (clk) {
        case CLOCK_REALTIME:
        case CLOCK_MONOTONIC:
        case CLOCK_MONOTONIC_RAW:
        case CLOCK_REALTIME_COARSE:
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

/* TODO: no timers */
static long sleep_until(uint64_t deadline) {
    while (rdtsc() < deadline) schedule();

    return 0;
}

static int timespec_to_tsc(long uts, uint64_t *tsc) {
    struct timespec ts;
    uint64_t freq = this_cpu_read(tsc_freq_hz);

    int err = copy_from_user(&ts, uts, sizeof(ts));
    if (err) return err;

    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || (uint64_t)ts.tv_nsec >= NSEC_PER_SEC) return -EINVAL;

    /* a year past now at most, so the sum cannot wrap */
    if ((uint64_t)ts.tv_sec > 365ULL * 24 * 3600) ts.tv_sec = 365ULL * 24 * 3600;

    *tsc = ts.tv_sec * freq + ts.tv_nsec * freq / NSEC_PER_SEC;
    return 0;
}

long sys_nanosleep(long ureq, long urem) {
    uint64_t delta;

    int err = timespec_to_tsc(ureq, &delta);
    if (err) return err;

    sleep_until(rdtsc() + delta);

    struct timespec zero = { 0, 0 };
    if (urem) return copy_to_user(urem, &zero, sizeof(zero));

    return 0;
}

long sys_clock_nanosleep(long clk, long flags, long ureq, long urem) {
    uint64_t when, now;

    if (clk == CLOCK_THREAD_CPUTIME_ID) return -EINVAL;

    int err = clock_tsc(clk, &now);
    if (err) return err;

    if ((err = timespec_to_tsc(ureq, &when)) != 0) return err;

    if (!(flags & TIMER_ABSTIME)) when += now;

    sleep_until(rdtsc() + (when > now ? when - now : 0));

    struct timespec zero = { 0, 0 };
    if (urem && !(flags & TIMER_ABSTIME)) return copy_to_user(urem, &zero, sizeof(zero));

    return 0;
}
