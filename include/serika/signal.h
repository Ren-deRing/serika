#pragma once

#include <serika/lock.h>

#include <stdbool.h>
#include <stdint.h>

#define SIGHUP     1
#define SIGINT     2
#define SIGQUIT    3
#define SIGILL     4
#define SIGTRAP    5
#define SIGABRT    6
#define SIGBUS     7
#define SIGFPE     8
#define SIGKILL    9
#define SIGUSR1   10
#define SIGSEGV   11
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20
#define SIGTTIN   21
#define SIGTTOU   22
#define SIGURG    23
#define SIGWINCH  28

#define NSIG      64

#define SIG_DFL   0
#define SIG_IGN   1

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#define SA_SIGINFO   0x00000004
#define SA_RESTORER  0x04000000
#define SA_ONSTACK   0x08000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000

/* si_code */
#define SI_USER     0
#define SI_KERNEL   0x80
#define SI_TKILL    (-6)
#define SEGV_MAPERR 1
#define SEGV_ACCERR 2
#define CLD_EXITED  1

#define sigmask(sig) (1ULL << ((sig) - 1))

/* never blocked, never caught */
#define SIG_UNBLOCKABLE (sigmask(SIGKILL) | sigmask(SIGSTOP))

/* the kernel's struct sigaction on x86_64 */
struct k_sigaction {
    uint64_t handler;  /* SIG_DFL, SIG_IGN or the user's function */
    uint64_t flags;    /* SA_* */
    uint64_t restorer; /* returns through rt_sigreturn */
    uint64_t mask;     /* blocked while the handler runs */
};

struct ksiginfo {
    int       signo;
    int       code;
    int       pid;
    uintptr_t addr;    /* faults */
};

/* handlers, shared by threads like files. the lock also covers pending and blocked of its tasks */
struct sighand {
    spinlock_t         lock;
    uint32_t           users; /* atomic */
    struct k_sigaction action[NSIG];
};

struct task;

struct sighand *sighand_create(void);
struct sighand *sighand_copy(struct sighand *old);
void signal_exec_reset(void);
void sighand_get(struct sighand *sh);
void sighand_put(struct sighand *sh);

int  send_signal(struct task *t, int sig, const struct ksiginfo *info);
void force_sig(int sig, const struct ksiginfo *info); /* the current task, even blocked or ignored */
bool signal_pending(struct task *t);

/* return to user: the next signal to act on, 0 for none */
int  signal_dequeue(struct ksiginfo *info, struct k_sigaction *ka);
void signal_delivered(int sig, const struct k_sigaction *ka);
void signal_set_blocked(uint64_t mask);

/* the mask of ppoll(): the call's own, back when it returns or, if a signal ended it, once that signal has its frame */
int  sigmask_swap(uintptr_t uset, size_t size);
void sigmask_unswap(long ret);
void sigmask_restore(void);

/* arch: the frame on the user stack, and back */
struct trapframe;

void exit_to_user(struct trapframe *tf, bool syscall); /* irq off */
bool signal_restore(struct trapframe *tf);              /* rt_sigreturn */
