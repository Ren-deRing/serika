#include <fenv.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* exits 0x5e if every check holds, else the number of the failed one.
   "abort" and "sleep" as argv[1] are for the kernel tests */

static volatile sig_atomic_t got_sig, got_code, got_pid;

static void on_usr1(int sig, siginfo_t *si, void *uc) {
    (void)uc;

    got_sig  = sig;
    got_code = si->si_code;
    got_pid  = si->si_pid;

    /* the handler's own fpu state: this must not leak back */
    fesetround(FE_UPWARD);
}

static sigjmp_buf jb;
static void *volatile fault_addr;

static void on_segv(int sig, siginfo_t *si, void *uc) {
    (void)sig; (void)uc;

    fault_addr = si->si_addr;
    siglongjmp(jb, 1);
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "abort") == 0) abort();
    if (argc > 1 && strcmp(argv[1], "sleep") == 0) {
        sleep(10);
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_usr1;
    sa.sa_flags = SA_SIGINFO;
    if (sigaction(SIGUSR1, &sa, NULL) != 0) return 1;

    /* a handler runs and returns, and the fpu state comes back */
    if (fegetround() != FE_TONEAREST) return 2;
    if (raise(SIGUSR1) != 0) return 3;
    if (got_sig != SIGUSR1 || got_code != SI_TKILL || got_pid != getpid()) return 4;
    if (fegetround() != FE_TONEAREST) return 5;

    /* blocked: pending until unblocked */
    sigset_t set, pend;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);

    got_sig = 0;
    if (sigprocmask(SIG_BLOCK, &set, NULL) != 0) return 6;
    raise(SIGUSR1);
    if (got_sig != 0) return 7;
    if (sigpending(&pend) != 0 || !sigismember(&pend, SIGUSR1)) return 8;
    if (sigprocmask(SIG_UNBLOCK, &set, NULL) != 0) return 9;
    if (got_sig != SIGUSR1) return 10;

    /* kill() is SI_USER */
    got_sig = 0;
    if (kill(getpid(), SIGUSR1) != 0 || got_sig != SIGUSR1 || got_code != SI_USER) return 11;

    /* ignored */
    if (signal(SIGUSR2, SIG_IGN) == SIG_ERR) return 12;
    raise(SIGUSR2);

    /* a fault with a handler, twice: siglongjmp puts the mask back */
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO;
    if (sigaction(SIGSEGV, &sa, NULL) != 0) return 13;

    for (int i = 0; i < 2; i++) {
        fault_addr = NULL;

        if (sigsetjmp(jb, 1) == 0) {
            *(volatile int *)0x1000 = 1;
            return 14;
        }

        if (fault_addr != (void *)0x1000) return 15;
    }

    /* 0 asks whether it is there */
    if (kill(getpid(), 0) != 0) return 16;
    if (kill(99999, SIGUSR1) != -1) return 17;

    printf("signals: all checks passed\n");
    return 0x5e;
}
