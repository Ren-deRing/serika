#include <serika/cpu.h>
#include <serika/errno.h>
#include <serika/mm.h>
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

/* a thread shares all of these. no cwd yet: CLONE_FS has nothing to share */
#define CLONE_THREAD_NEEDS (CLONE_VM | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD)

/* sharing without being a thread, and vfork's wait: not yet */
#define CLONE_UNSUPPORTED (CLONE_VFORK | CLONE_PARENT)

/* x86_64's order: flags, stack, parent_tid, child_tid, tls. glibc's fork() comes here too */
long sys_clone(long flags, long newsp, long ptid, long ctid, long tls) {
    struct task *curr = current_task();
    struct task *child;

    bool thread = flags & CLONE_THREAD;

    /* a thread: all of it shared, no exit signal, its own stack. a process: none of it shared */
    bool ok = thread ? (flags & CLONE_THREAD_NEEDS) == CLONE_THREAD_NEEDS && !(flags & CSIGNAL) && newsp
                     : !(flags & CLONE_THREAD_NEEDS) && (!(flags & CSIGNAL) || (flags & CSIGNAL) == SIGCHLD);

    if (!ok || (flags & CLONE_UNSUPPORTED)) {
        printk("syscall: clone flags 0x%lx not supported -- task %lu\n", flags, curr->id);
        return -EINVAL;
    }

    if ((flags & CLONE_SETTLS) && (uint64_t)tls >= USER_SPACE_END) return -EPERM;

    int err = thread ? task_clone_thread(curr->tf, &child) : task_fork(curr->tf, &child);
    if (err) return err;

    if (newsp) child->tf->rsp = newsp;
    if (flags & CLONE_SETTLS) child->arch.fs_base = tls;

    int32_t pid = child->id;

    /* in the child's memory, which a thread shares with us: before it runs */
    if (flags & CLONE_CHILD_SETTID) mm_write(child->mm, ctid, &pid, sizeof(pid));
    if (flags & CLONE_PARENT_SETTID) copy_to_user(ptid, &pid, sizeof(pid));

    /* zeroed and woken when it exits: pthread_join() */
    if (flags & CLONE_CHILD_CLEARTID) child->clear_child_tid = ctid;

    sched_add_task(child);
    return pid;
}

long sys_fork(void) {
    return sys_clone(SIGCHLD, 0, 0, 0, 0);
}

/* a fork is a correct vfork: the caller just does not wait for the exec */
long sys_vfork(void) {
    return sys_clone(SIGCHLD, 0, 0, 0, 0);
}

long sys_wait4(long pid, long ustatus, long options, long urusage) {
    if (pid == 0 || pid < -1) {
        printk("syscall: wait4 for pid %ld not supported -- task %lu\n", pid, current_task()->id);
        return -EINVAL; /* no process groups yet */
    }

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
