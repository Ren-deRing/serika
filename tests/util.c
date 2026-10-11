#include <serika/arch.h>
#include <serika/cpu.h>
#include <serika/exec.h>
#include <serika/printk.h>
#include <serika/sched.h>
#include <serika/syscall.h>

#include <asm/arch.h>

#include <tests/test.h>

/* reaped id, 0 on timeout */
long reap_within(struct task *t, long *status, uint64_t ms) {
    long reaped = 0;
    uint64_t until = rdtsc() + curcpu->tsc_freq_hz / 1000 * ms;

    while ((reaped = task_wait(t, status, WNOHANG)) == 0 && rdtsc() < until) arch_pause();

    return reaped;
}

/* reaped id, 0 on timeout */
long run_user(const uint8_t *c, size_t size, long *status) {
    struct task *t = task_create_user(c, size, 0);
    if (!t) return 0;

    sched_add_task(t);

    return reap_within(t, status, 2000);
}

/* the exit code on failure tells which check of the program failed */
long run_exec(const char *path, long *status) {
    const char *const argv[] = { path, "foo", "bar", NULL };
    const char *const envp[] = { "X=1", NULL };
    struct task *t;

    if (task_create_exec(path, argv, envp, 0, &t) != 0) return 0;

    sched_add_task(t);

    long reaped = reap_within(t, status, 2000);

    if (reaped > 0 && WEXITSTATUS(*status) != 0x5e)
        printk("exec: %s exited with %ld\n", path, WEXITSTATUS(*status));

    return reaped;
}

