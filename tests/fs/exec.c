#include <serika/errno.h>
#include <serika/exec.h>
#include <serika/sched.h>
#include <serika/signal.h>
#include <serika/syscall.h>

#include <tests/test.h>

/* reaped id, 0 on timeout */
static long reap_within(struct task *t, long *status, uint64_t ms) {
    long reaped = 0;
    uint64_t until = rdtsc() + curcpu->tsc_freq_hz / 1000 * ms;

    while ((reaped = task_wait(t, status, WNOHANG)) == 0 && rdtsc() < until) arch_pause();

    return reaped;
}

/* the exit code on failure tells which check of the program failed */
static long run_exec(const char *path, long *status) {
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

bool test_exec_glibc() {
    long status = 0;

    TEST_ASSERT(run_exec("/bin/hello", &status) > 0);
    TEST_ASSERT(WEXITSTATUS(status) == 0x5e);
    return true;
}

/* files, the console, time and munmap, through glibc */
bool test_exec_files() {
    long status = 0;

    TEST_ASSERT(run_exec("/bin/files", &status) > 0);
    TEST_ASSERT(WEXITSTATUS(status) == 0x5e);
    return true;
}

/* handlers, masks, kill and faults, through glibc. then the default actions */
bool test_exec_signals() {
    long status = 0;
    struct task *t;

    TEST_ASSERT(run_exec("/bin/signals", &status) > 0);
    TEST_ASSERT(WEXITSTATUS(status) == 0x5e);

    const char *const abort_argv[] = { "/bin/signals", "abort", NULL };
    TEST_ASSERT(task_create_exec("/bin/signals", abort_argv, NULL, 0, &t) == 0);
    sched_add_task(t);
    TEST_ASSERT(reap_within(t, &status, 2000) > 0);
    TEST_ASSERT(status == SIGABRT);

    /* asleep in the kernel: the signal ends the sleep, then the task */
    const char *const sleep_argv[] = { "/bin/signals", "sleep", NULL };
    TEST_ASSERT(task_create_exec("/bin/signals", sleep_argv, NULL, 0, &t) == 0);
    sched_add_task(t);

    uint64_t until = rdtsc() + curcpu->tsc_freq_hz / 10; /* 100ms to get there */
    while (rdtsc() < until) arch_pause();

    struct ksiginfo info = { .signo = SIGTERM, .code = SI_USER };
    TEST_ASSERT(send_signal(t, SIGTERM, &info) == 0);
    TEST_ASSERT(reap_within(t, &status, 1000) > 0);
    TEST_ASSERT(status == SIGTERM);
    return true;
}
