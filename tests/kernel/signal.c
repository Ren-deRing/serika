#include <serika/arch.h>
#include <serika/cpu.h>
#include <serika/exec.h>
#include <serika/sched.h>
#include <serika/signal.h>
#include <serika/syscall.h>

#include <asm/arch.h>

#include <tests/test.h>

/* all of it through /bin/signals (user/glibc/signals.c). argv[1] is how it behaves */
static struct task *spawn_mode(const char *mode) {
    const char *const argv[] = { "/bin/signals", mode, NULL };
    struct task *t;

    if (task_create_exec("/bin/signals", argv, NULL, 0, &t) != 0) return NULL;

    sched_add_task(t);
    return t;
}

/* it ends by itself, with this status */
static bool ends_with(const char *mode, long want) {
    long status = 0;
    struct task *t = spawn_mode(mode);

    return t && reap_within(t, &status, 2000) > 0 && status == want;
}

/* it never ends by itself: wait until it is there, send sig, and it ends with sig */
static bool killed_by(const char *mode, int sig, uint64_t settle_ms) {
    long status = 0;
    struct task *t = spawn_mode(mode);
    if (!t) return false;

    uint64_t until = rdtsc() + curcpu->tsc_freq_hz / 1000 * settle_ms;
    while (rdtsc() < until) arch_pause();

    struct ksiginfo info = { .signo = sig, .code = SI_USER };

    return send_signal(t, sig, &info) == 0 && reap_within(t, &status, 1000) > 0 && status == sig;
}

/* handlers, masks, kill and a fault with a handler, then the default action of abort() */
bool test_signal_glibc() {
    long status = 0;

    TEST_ASSERT(run_exec("/bin/signals", &status) > 0);
    TEST_ASSERT(WEXITSTATUS(status) == 0x5e);

    TEST_ASSERT(ends_with("abort", SIGABRT));
    return true;
}

/* an exception becomes a signal, and with no handler it ends the task */
bool test_signal_fault() {
    TEST_ASSERT(ends_with("segv", SIGSEGV));
    TEST_ASSERT(ends_with("ill", SIGILL));
    TEST_ASSERT(ends_with("fpe", SIGFPE));
    TEST_ASSERT(ends_with("trap", SIGTRAP));
    return true;
}

/* sent from outside: to a task running in user mode on some cpu, and to one asleep in the kernel */
bool test_signal_kill() {
    TEST_ASSERT(killed_by("spin", SIGKILL, 50));
    TEST_ASSERT(killed_by("sleep", SIGTERM, 100));
    return true;
}
