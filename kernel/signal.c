#include <serika/cpu.h>
#include <serika/errno.h>
#include <serika/mm.h>
#include <serika/preempt.h>
#include <serika/printk.h>
#include <serika/sched.h>
#include <serika/signal.h>
#include <serika/slab.h>
#include <serika/string.h>
#include <serika/syscall.h>
#include <serika/task.h>

struct sighand *sighand_create(void) {
    struct sighand *sh = kmalloc(sizeof(struct sighand));
    if (!sh) return NULL;

    memset(sh, 0, sizeof(*sh)); /* every action SIG_DFL */
    spin_lock_init(&sh->lock);
    sh->users = 1;

    return sh;
}

/* fork: the same handlers */
struct sighand *sighand_copy(struct sighand *old) {
    struct sighand *sh = sighand_create();
    if (!sh) return NULL;

    uint64_t flags = spin_lock_irqsave(&old->lock);
    memcpy(sh->action, old->action, sizeof(sh->action));
    spin_unlock_irqrestore(&old->lock, flags);

    return sh;
}

/* execve: the new program has none of the old one's functions. what was ignored stays ignored */
/* every handler back to the default, except what is ignored */
void sighand_reset(struct sighand *sh) {
    uint64_t flags = spin_lock_irqsave(&sh->lock);

    for (int i = 0; i < NSIG; i++)
        if (sh->action[i].handler != SIG_IGN) sh->action[i] = (struct k_sigaction){ .handler = SIG_DFL };

    spin_unlock_irqrestore(&sh->lock, flags);
}

void signal_exec_reset(void) {
    sighand_reset(current_task()->sighand);
}

void sighand_get(struct sighand *sh) {
    __atomic_add_fetch(&sh->users, 1, 0); /* __ATOMIC_RELAXED */
}

void sighand_put(struct sighand *sh) {
    if (__atomic_sub_fetch(&sh->users, 1, 4) != 0) return; /* __ATOMIC_ACQ_REL */

    kfree(sh);
}

/* SIG_DFL does nothing for these. no job control yet: the stop signals too */
static bool default_ignored(int sig) {
    switch (sig) {
        case SIGCHLD: case SIGURG: case SIGWINCH: case SIGCONT:
        case SIGSTOP: case SIGTSTP: case SIGTTIN: case SIGTTOU:
            return true;
    }

    return false;
}

static bool is_stop(int sig) {
    return sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU;
}

/* sighand lock held */
static bool ignored(struct task *t, int sig) {
    uint64_t handler = t->sighand->action[sig - 1].handler;

    if (t->blocked & sigmask(sig)) return false; /* may be unblocked with a handler by then */

    return handler == SIG_IGN || (handler == SIG_DFL && default_ignored(sig) && !is_stop(sig));
}

bool signal_pending(struct task *t) {
    uint64_t pending = __atomic_load_n(&t->pending, 2); /* __ATOMIC_ACQUIRE */
    uint64_t blocked = __atomic_load_n(&t->blocked, 0); /* __ATOMIC_RELAXED */

    return (pending & ~blocked) != 0;
}

/* a sleeper wakes and finds it, a runner on another cpu comes into the kernel and finds it */
static void signal_wake(struct task *t) {
    if (!wake_up_process(t)) sched_kick(t);
}

int send_signal(struct task *t, int sig, const struct ksiginfo *info) {
    struct sighand *sh = t->sighand;
    if (!sh) return -EPERM; /* kthreads take no signals */

    uint64_t flags = spin_lock_irqsave(&sh->lock);

    bool queued = !ignored(t, sig);

    if (queued) {
        t->siginfo[sig - 1] = *info;
        __atomic_or_fetch(&t->pending, sigmask(sig), 5); /* __ATOMIC_SEQ_CST. a sleeper checks it after setting its state */
    }

    spin_unlock_irqrestore(&sh->lock, flags);

    if (queued) signal_wake(t);
    return 0;
}

/* an exception: a handler gets it if there is one, else the task dies even if it blocked or ignored it */
void force_sig(int sig, const struct ksiginfo *info) {
    struct task *curr = current_task();
    struct sighand *sh = curr->sighand;

    uint64_t flags = spin_lock_irqsave(&sh->lock);

    struct k_sigaction *ka = &sh->action[sig - 1];

    if ((curr->blocked & sigmask(sig)) || ka->handler == SIG_IGN) {
        curr->blocked &= ~sigmask(sig);
        ka->handler = SIG_DFL;
    }

    curr->siginfo[sig - 1] = *info;
    __atomic_or_fetch(&curr->pending, sigmask(sig), 5); /* __ATOMIC_SEQ_CST */

    spin_unlock_irqrestore(&sh->lock, flags);
}

/* ignored ones are dropped on the way. the lowest number goes first */
int signal_dequeue(struct ksiginfo *info, struct k_sigaction *ka) {
    struct task *curr = current_task();
    struct sighand *sh = curr->sighand;
    int sig = 0;

    if (!sh) return 0;

    uint64_t flags = spin_lock_irqsave(&sh->lock);

    for (;;) {
        uint64_t ready = curr->pending & ~curr->blocked;
        if (!ready) {
            sig = 0;
            break;
        }

        sig = __builtin_ctzll(ready) + 1;
        __atomic_and_fetch(&curr->pending, ~sigmask(sig), 5); /* __ATOMIC_SEQ_CST */

        *info = curr->siginfo[sig - 1];
        *ka = sh->action[sig - 1];

        if (ka->handler == SIG_IGN) continue;

        if (ka->handler == SIG_DFL) {
            if (is_stop(sig)) printk("signal: task %lu ignores %d -- no job control yet\n", curr->id, sig);
            if (default_ignored(sig)) continue;
            break;
        }

        if (ka->flags & SA_RESETHAND) sh->action[sig - 1].handler = SIG_DFL;
        break;
    }

    spin_unlock_irqrestore(&sh->lock, flags);
    return sig;
}

/* the frame is on the user stack: what the handler runs with is blocked now */
void signal_delivered(int sig, const struct k_sigaction *ka) {
    uint64_t mask = ka->mask;
    if (!(ka->flags & SA_NODEFER)) mask |= sigmask(sig);

    signal_set_blocked(current_task()->blocked | mask);
}

void signal_set_blocked(uint64_t mask) {
    struct task *curr = current_task();

    uint64_t flags = spin_lock_irqsave(&curr->sighand->lock);
    __atomic_store_n(&curr->blocked, mask & ~SIG_UNBLOCKABLE, 0); /* __ATOMIC_RELAXED */
    spin_unlock_irqrestore(&curr->sighand->lock, flags);
}

int sigmask_swap(uintptr_t uset, size_t size) {
    struct task *curr = current_task();
    uint64_t mask;

    if (size != sizeof(mask)) return -EINVAL;
    if (copy_from_user(&mask, uset, sizeof(mask)) != 0) return -EFAULT;

    curr->saved_blocked = curr->blocked;
    curr->restore_sigmask = true;

    signal_set_blocked(mask);
    return 0;
}

void sigmask_restore(void) {
    struct task *curr = current_task();

    if (!curr->restore_sigmask) return;

    curr->restore_sigmask = false;
    signal_set_blocked(curr->saved_blocked);
}

/* -EINTR: a signal may be waiting that only the call's mask lets in. exit_to_user() restores after it */
void sigmask_unswap(long ret) {
    if (ret != -EINTR) sigmask_restore();
}

/* until a signal comes. never restarted */
static long wait_for_signal(void) {
    struct wait_queue wq;

    wait_queue_init(&wq);

    wait_event_interruptible(wq, false);
    return -EINTR;
}

long sys_pause(void) {
    return wait_for_signal();
}

/* the mask is the call's own until the handler of the signal that ends it is set up */
long sys_rt_sigsuspend(long uset, long size) {
    int err = sigmask_swap(uset, size);
    if (err) return err;

    return wait_for_signal();
}

long sys_rt_sigaction(long sig, long act, long oact, long sigsetsize) {
    struct task *curr = current_task();
    struct sighand *sh = curr->sighand;
    struct k_sigaction new, old;

    if (sigsetsize != sizeof(uint64_t)) return -EINVAL;
    if (sig < 1 || sig > NSIG) return -EINVAL;
    if (act && (sig == SIGKILL || sig == SIGSTOP)) return -EINVAL;

    if (act && copy_from_user(&new, act, sizeof(new)) != 0) return -EFAULT;

    uint64_t flags = spin_lock_irqsave(&sh->lock);

    old = sh->action[sig - 1];

    if (act) {
        new.mask &= ~SIG_UNBLOCKABLE;
        sh->action[sig - 1] = new;

        /* pending and now ignored */
        if (new.handler == SIG_IGN || (new.handler == SIG_DFL && default_ignored(sig)))
            __atomic_and_fetch(&curr->pending, ~sigmask(sig), 5); /* __ATOMIC_SEQ_CST */
    }

    spin_unlock_irqrestore(&sh->lock, flags);

    if (oact && copy_to_user(oact, &old, sizeof(old)) != 0) return -EFAULT;
    return 0;
}

long sys_rt_sigprocmask(long how, long set, long oset, long sigsetsize) {
    struct task *curr = current_task();
    uint64_t old = curr->blocked, new;

    if (sigsetsize != sizeof(uint64_t)) return -EINVAL;

    if (set) {
        if (copy_from_user(&new, set, sizeof(new)) != 0) return -EFAULT;

        if (how == SIG_BLOCK)        new = old | new;
        else if (how == SIG_UNBLOCK) new = old & ~new;
        else if (how != SIG_SETMASK) return -EINVAL;

        /* a signal unblocked here is delivered on the way out of this call */
        signal_set_blocked(new);
    }

    if (oset && copy_to_user(oset, &old, sizeof(old)) != 0) return -EFAULT;
    return 0;
}

long sys_rt_sigpending(long uset, long sigsetsize) {
    struct task *curr = current_task();

    if (sigsetsize != sizeof(uint64_t)) return -EINVAL;

    uint64_t set = __atomic_load_n(&curr->pending, 2) & curr->blocked; /* __ATOMIC_ACQUIRE */
    return copy_to_user(uset, &set, sizeof(set));
}

long sys_getpid(void) {
    struct task *curr = current_task();
    return curr->group ? (long)curr->group->tgid : (long)curr->id;
}

long sys_gettid(void) {
    return current_task()->id;
}

static long kill_one(long pid, long sig, int code) {
    struct ksiginfo info = { .signo = sig, .code = code, .pid = sys_getpid() };

    if (sig < 0 || sig > NSIG) return -EINVAL;

    return task_signal(pid, sig, &info);
}

long sys_kill(long pid, long sig) {
    if (pid == -1) {
        printk("syscall: kill to every process not supported -- task %lu\n", current_task()->id);
        return -ENOSYS;
    }

    if (pid > 0) return kill_one(pid, sig, SI_USER);

    /* 0: the caller's group, below -1: group -pid */
    struct ksiginfo info = { .signo = sig, .code = SI_USER, .pid = sys_getpid() };

    if (sig < 0 || sig > NSIG) return -EINVAL;

    return task_signal_pgrp(pid == 0 ? sys_getpgrp() : -pid, sig, &info);
}

/* that thread, wherever its process is */
long sys_tkill(long tid, long sig) {
    struct ksiginfo info = { .signo = sig, .code = SI_TKILL, .pid = sys_getpid() };

    if (tid <= 0 || sig < 0 || sig > NSIG) return -EINVAL;

    return task_signal_thread(0, tid, sig, &info);
}

/* that thread, if it is in process tgid */
long sys_tgkill(long tgid, long tid, long sig) {
    struct ksiginfo info = { .signo = sig, .code = SI_TKILL, .pid = sys_getpid() };

    if (tgid <= 0 || tid <= 0 || sig < 0 || sig > NSIG) return -EINVAL;

    return task_signal_thread(tgid, tid, sig, &info);
}
