#include <serika/errno.h>
#include <serika/exec.h>
#include <serika/sched.h>
#include <serika/syscall.h>

#include <tests/test.h>

/* reaped id, 0 on timeout. the exit code on failure tells which check of check.c failed */
static long run_exec(const char *path, long *status) {
    const char *const argv[] = { path, "foo", "bar", NULL };
    const char *const envp[] = { "X=1", NULL };
    struct task *t;

    if (task_create_exec(path, argv, envp, 0, &t) != 0) return 0;

    sched_add_task(t);

    long reaped = 0;
    uint64_t until = rdtsc() + curcpu->tsc_freq_hz * 2; /* 2s */

    while ((reaped = task_wait(t, status, WNOHANG)) == 0 && rdtsc() < until) arch_pause();

    if (reaped > 0 && WEXITSTATUS(*status) != 0x5e)
        printk("exec: %s exited with %ld\n", path, WEXITSTATUS(*status));

    return reaped;
}

bool test_exec() {
    long status = 0;
    struct task *t;

    TEST_ASSERT(run_exec("/bin/check", &status) > 0);
    TEST_ASSERT(WEXITSTATUS(status) == 0x5e);

    /* static-pie, at ELF_ET_DYN_BASE */
    TEST_ASSERT(run_exec("/bin/check-pie", &status) > 0);
    TEST_ASSERT(WEXITSTATUS(status) == 0x5e);

    TEST_ASSERT(task_create_exec("/bin/nope", NULL, NULL, 0, &t) == -ENOENT);
    TEST_ASSERT(task_create_exec("/etc/motd", NULL, NULL, 0, &t) == -ENOEXEC);
    TEST_ASSERT(task_create_exec("/bin", NULL, NULL, 0, &t) == -EACCES);
    return true;
}
