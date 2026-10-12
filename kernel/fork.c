#include <serika/cpu.h>
#include <serika/errno.h>
#include <serika/mm.h>
#include <serika/preempt.h>
#include <serika/printk.h>
#include <serika/sched.h>
#include <serika/signal.h>
#include <serika/string.h>
#include <serika/syscall.h>
#include <serika/task.h>

#include <asm/task.h>
#include <asm/trapframe.h>

/* linux values */
#define CSIGNAL              0x000000ff
#define CLONE_VM             0x00000100
#define CLONE_FS             0x00000200
#define CLONE_FILES          0x00000400
#define CLONE_SIGHAND        0x00000800
#define CLONE_VFORK          0x00004000
#define CLONE_PARENT         0x00008000
#define CLONE_THREAD         0x00010000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID   0x01000000
#define CLONE_CLEAR_SIGHAND  0x100000000ULL /* clone3 only */

/* a thread shares all of these. no cwd yet: CLONE_FS has nothing to share */
#define CLONE_THREAD_NEEDS (CLONE_VM | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD)

/* sharing without being a thread: not yet. the mm only goes to a parent that waits for the child to leave it */
#define CLONE_UNSUPPORTED (CLONE_PARENT)

/* x86_64's order: flags, stack, parent_tid, child_tid, tls. glibc's fork() comes here too */
long sys_clone(long flags, long newsp, long ptid, long ctid, long tls) {
    struct task *curr = current_task();
    struct task *child;

    bool thread = flags & CLONE_THREAD;
    bool vfork = flags & CLONE_VFORK;

    /* a thread: all of it shared, no exit signal, its own stack. a process: only its mm, and then only while
     * the parent waits (vfork), nothing else */
    bool ok = thread ? (flags & CLONE_THREAD_NEEDS) == CLONE_THREAD_NEEDS && !(flags & CSIGNAL) && newsp && !vfork
                                                                                && !(flags & CLONE_CLEAR_SIGHAND)
                     : !(flags & (CLONE_FILES | CLONE_SIGHAND)) && (!(flags & CLONE_VM) || vfork)
                       && (!(flags & CSIGNAL) || (flags & CSIGNAL) == SIGCHLD);

    if (!ok || (flags & CLONE_UNSUPPORTED)) {
        printk("syscall: clone flags 0x%lx not supported -- task %lu\n", flags, curr->id);
        return -EINVAL;
    }

    if ((flags & CLONE_SETTLS) && (uint64_t)tls >= USER_SPACE_END) return -EPERM;

    struct vfork_done *vd = vfork ? vfork_alloc() : NULL;
    if (vfork && !vd) return -ENOMEM;

    int err = thread ? task_clone_thread(curr->tf, &child)
                     : task_fork(curr->tf, flags & CLONE_VM, flags & CLONE_CLEAR_SIGHAND, &child);
    if (err) {
        if (vd) vfork_cancel(vd);
        return err;
    }

    if (vd) vfork_attach(child, vd);

    if (newsp) child->tf->rsp = newsp;
    if (flags & CLONE_SETTLS) child->arch.fs_base = tls;

    int32_t pid = child->id;

    /* in the child's memory, which a thread shares with us: before it runs */
    if (flags & CLONE_CHILD_SETTID) mm_write(child->mm, ctid, &pid, sizeof(pid));
    if (flags & CLONE_PARENT_SETTID) copy_to_user(ptid, &pid, sizeof(pid));

    /* zeroed and woken when it exits: pthread_join() */
    if (flags & CLONE_CHILD_CLEARTID) child->clear_child_tid = ctid;

    sched_add_task(child);

    /* until the child execs or exits: it runs on our memory */
    if (vd) vfork_wait(vd);

    return pid;
}

/* linux's struct clone_args, the part this takes */
struct clone_args {
    uint64_t flags;
    uint64_t pidfd;
    uint64_t child_tid;
    uint64_t parent_tid;
    uint64_t exit_signal;
    uint64_t stack;
    uint64_t stack_size;
    uint64_t tls;
};

#define CLONE_ARGS_SIZE_VER0 64
#define CLONE_PIDFD          0x00001000

/* the same as clone(), with the arguments in a struct and the stack as a base and a size. glibc tries it first */
long sys_clone3(long uargs, long size) {
    struct clone_args a = { 0 };

    if (size < CLONE_ARGS_SIZE_VER0 || size > (long)PAGE_SIZE) return -EINVAL;

    /* fields after these are for features not here. a newer program must not have them set */
    int err = copy_from_user(&a, uargs, sizeof(a));
    if (err) return err;

    if (size > (long)sizeof(a)) {
        uint8_t rest;

        for (long off = sizeof(a); off < size; off++) {
            if (copy_from_user(&rest, uargs + off, 1) != 0) return -EFAULT;
            if (rest) return -E2BIG;
        }
    }

    if ((a.flags & CSIGNAL) || (a.flags & CLONE_PIDFD) || a.exit_signal > CSIGNAL) return -EINVAL;
    if (!a.stack != !a.stack_size) return -EINVAL;

    return sys_clone(a.flags | a.exit_signal, a.stack ? a.stack + a.stack_size : 0, a.parent_tid, a.child_tid, a.tls);
}

long sys_fork(void) {
    return sys_clone(SIGCHLD, 0, 0, 0, 0);
}

/* a fork is a correct vfork: the caller just does not wait for the exec */
long sys_vfork(void) {
    return sys_clone(SIGCHLD, 0, 0, 0, 0);
}

long sys_wait4(long pid, long ustatus, long options, long urusage) {
    long status = 0;
    long ret = task_waitpid(pid, &status, options & WNOHANG);
    if (ret <= 0) return ret;

    int32_t s = status;
    if (ustatus && copy_to_user(ustatus, &s, sizeof(s)) != 0) return -EFAULT;

    /* no accounting yet: all zero */
    if (urusage) {
        uint8_t zero[144] = { 0 };
        if (copy_to_user(urusage, zero, sizeof(zero)) != 0) return -EFAULT;
    }

    return ret;
}
