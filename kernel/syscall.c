#include <serika/cpu.h>
#include <serika/errno.h>
#include <serika/mm.h>
#include <serika/printk.h>
#include <serika/syscall.h>
#include <serika/task.h>

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20

static long sys_exit(long code) {
    task_exit((code & 0xff) << 8);
    return 0;
}

/* TODO: no fd table yet */
static long sys_write(long fd, long buf, long count) {
    char kbuf[256];
    long done = 0;

    if (fd != 1 && fd != 2) return -EBADF;
    if (count < 0) return -EINVAL;

    while (done < count) {
        size_t n = count - done;
        if (n > sizeof(kbuf)) n = sizeof(kbuf);

        int err = copy_from_user(kbuf, buf + done, n);
        if (err) return done ? done : err;

        printk("%.*s", (int)n, kbuf);
        done += n;
    }

    return done;
}

/* TODO: private anonymous only */
static long sys_mmap(long addr, long len, long prot, long flags, long fd, long off) {
    (void)addr; (void)fd; (void)off;

    if ((flags & (MAP_SHARED | MAP_PRIVATE)) != MAP_PRIVATE || !(flags & MAP_ANONYMOUS) || (flags & MAP_FIXED)) {
        printk("syscall: mmap flags 0x%lx not supported -- task %lu\n", flags, current_task()->id);
        return -EINVAL;
    }

    return mm_mmap(current_task()->mm, len, prot);
}

static long sys_mprotect(long addr, long len, long prot) {
    return mm_protect(current_task()->mm, addr, len, prot);
}

static long sys_brk(long brk) {
    return mm_brk(current_task()->mm, brk);
}

/* TODO: no signals yet */
static long sys_rt_sigaction(long sig, long act, long oact, long sigsetsize) {
    (void)sig; (void)act;

    struct { uint64_t handler, flags, restorer, mask; } old = { 0 };

    if (sigsetsize != sizeof(uint64_t)) return -EINVAL;
    if (oact && copy_to_user(oact, &old, sizeof(old)) != 0) return -EFAULT;

    return 0;
}

/* TODO: same */
static long sys_rt_sigprocmask(long how, long set, long oset, long sigsetsize) {
    (void)how; (void)set;

    uint64_t old = 0;

    if (sigsetsize != sizeof(uint64_t)) return -EINVAL;
    if (oset && copy_to_user(oset, &old, sizeof(old)) != 0) return -EFAULT;

    return 0;
}

/* TODO: no futexes */
static long sys_set_tid_address(long tidptr) {
    (void)tidptr;

    return current_task()->id;
}

long syscall_dispatch(long nr, const long args[6]) {
    switch (nr) {
        case SYS_WRITE:           return sys_write(args[0], args[1], args[2]);
        case SYS_MMAP:            return sys_mmap(args[0], args[1], args[2], args[3], args[4], args[5]);
        case SYS_MPROTECT:        return sys_mprotect(args[0], args[1], args[2]);
        case SYS_BRK:             return sys_brk(args[0]);
        case SYS_RT_SIGACTION:    return sys_rt_sigaction(args[0], args[1], args[2], args[3]);
        case SYS_RT_SIGPROCMASK:  return sys_rt_sigprocmask(args[0], args[1], args[2], args[3]);
        case SYS_EXIT:            return sys_exit(args[0]);
        case SYS_ARCH_PRCTL:      return sys_arch_prctl(args[0], args[1]);
        case SYS_SET_TID_ADDRESS: return sys_set_tid_address(args[0]);
        case SYS_EXIT_GROUP:      return sys_exit(args[0]); /* no thread groups yet */
    }

    printk("syscall: %ld not implemented -- task %lu\n", nr, current_task()->id);
    return -ENOSYS;
}
