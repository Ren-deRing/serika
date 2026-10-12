#include <serika/cpu.h>
#include <serika/errno.h>
#include <serika/fcntl.h>
#include <serika/mm.h>
#include <serika/printk.h>
#include <serika/stat.h>
#include <serika/syscall.h>
#include <serika/task.h>
#include <serika/vfs.h>

static long sys_exit(long code) {
    task_exit((code & 0xff) << 8);
    return 0;
}

/* private only: anonymous, or a file read in page by page as it is touched */
static long sys_mmap(long addr, long len, long prot, long flags, long fd, long off) {
    struct file *file = NULL;

    if ((flags & (MAP_SHARED | MAP_PRIVATE)) != MAP_PRIVATE) {
        printk("syscall: mmap flags 0x%lx not supported -- task %lu\n", flags, current_task()->id);
        return -EINVAL;
    }

    if (!(flags & MAP_ANONYMOUS)) {
        if (!(file = fd_get(fd))) return -EBADF;

        if (!S_ISREG(file->inode->mode) || (file->flags & O_ACCMODE) == O_WRONLY) {
            vfs_close(file);
            return -EACCES;
        }
    }

    long ret = mm_mmap(current_task()->mm, addr, len, prot, flags, file, file ? off : 0);

    if (file) vfs_close(file);
    return ret;
}


static long sys_munmap(long addr, long len) {
    return mm_unmap(current_task()->mm, addr, len);
}

static long sys_mprotect(long addr, long len, long prot) {
    return mm_protect(current_task()->mm, addr, len, prot);
}

static long sys_brk(long brk) {
    return mm_brk(current_task()->mm, brk);
}

/* zeroed and futex-woken when this thread exits */
static long sys_set_tid_address(long tidptr) {
    current_task()->clear_child_tid = tidptr;
    return current_task()->id;
}

static long sys_exit_group(long code) {
    task_group_exit((code & 0xff) << 8);
    return 0;
}

long syscall_dispatch(long nr, const long args[6]) {
    switch (nr) {
        case SYS_READ:            return sys_read(args[0], args[1], args[2]);
        case SYS_WRITE:           return sys_write(args[0], args[1], args[2]);
        case SYS_OPEN:            return sys_openat(AT_FDCWD, args[0], args[1], args[2]);
        case SYS_CLOSE:           return sys_close(args[0]);
        case SYS_STAT:            return sys_newfstatat(AT_FDCWD, args[0], args[1], 0);
        case SYS_FSTAT:           return sys_fstat(args[0], args[1]);
        case SYS_LSTAT:           return sys_newfstatat(AT_FDCWD, args[0], args[1], 0); /* no links */
        case SYS_LSEEK:           return sys_lseek(args[0], args[1], args[2]);
        case SYS_MMAP:            return sys_mmap(args[0], args[1], args[2], args[3], args[4], args[5]);
        case SYS_MPROTECT:        return sys_mprotect(args[0], args[1], args[2]);
        case SYS_MUNMAP:          return sys_munmap(args[0], args[1]);
        case SYS_BRK:             return sys_brk(args[0]);
        case SYS_RT_SIGACTION:    return sys_rt_sigaction(args[0], args[1], args[2], args[3]);
        case SYS_RT_SIGPROCMASK:  return sys_rt_sigprocmask(args[0], args[1], args[2], args[3]);
        case SYS_IOCTL:           return sys_ioctl(args[0], args[1], args[2]);
        case SYS_PREAD64:         return sys_pread64(args[0], args[1], args[2], args[3]);
        case SYS_READV:           return sys_readv(args[0], args[1], args[2]);
        case SYS_WRITEV:          return sys_writev(args[0], args[1], args[2]);
        case SYS_ACCESS:          return sys_access(args[0], args[1]);
        case SYS_PIPE:            return sys_pipe2(args[0], 0);
        case SYS_DUP:             return sys_dup(args[0]);
        case SYS_DUP2:            return sys_dup2(args[0], args[1]);
        case SYS_NANOSLEEP:       return sys_nanosleep(args[0], args[1]);
        case SYS_GETPID:          return sys_getpid();
        case SYS_CLONE:           return sys_clone(args[0], args[1], args[2], args[3], args[4]);
        case SYS_FORK:            return sys_fork();
        case SYS_VFORK:           return sys_vfork();
        case SYS_EXECVE:          return sys_execve(args[0], args[1], args[2]);
        case SYS_EXIT:            return sys_exit(args[0]);
        case SYS_WAIT4:           return sys_wait4(args[0], args[1], args[2], args[3]);
        case SYS_KILL:            return sys_kill(args[0], args[1]);
        case SYS_UNAME:           return sys_uname(args[0]);
        case SYS_FCNTL:           return sys_fcntl(args[0], args[1], args[2]);
        case SYS_GETCWD:          return sys_getcwd(args[0], args[1]);
        case SYS_CHDIR:           return sys_chdir(args[0]);
        case SYS_MKDIR:           return sys_mkdirat(AT_FDCWD, args[0], args[1]);
        case SYS_READLINK:        return sys_readlinkat(AT_FDCWD, args[0], args[1], args[2]);
        case SYS_UMASK:           return sys_umask(args[0]);
        case SYS_GETUID:
        case SYS_GETGID:
        case SYS_GETEUID:
        case SYS_GETEGID:         return sys_getuid();
        case SYS_SETUID:
        case SYS_SETGID:          return sys_setuid(args[0]);
        case SYS_GETPPID:         return sys_getppid();
        case SYS_GETPGRP:         return sys_getpgrp();
        case SYS_RT_SIGPENDING:   return sys_rt_sigpending(args[0], args[1]);
        case SYS_ARCH_PRCTL:      return sys_arch_prctl(args[0], args[1]);
        case SYS_GETTID:          return sys_gettid();
        case SYS_TKILL:           return sys_tkill(args[0], args[1]);
        case SYS_FUTEX:           return sys_futex(args[0], args[1], args[2], args[3], args[4], args[5]);
        case SYS_GETDENTS64:      return sys_getdents64(args[0], args[1], args[2]);
        case SYS_SET_TID_ADDRESS: return sys_set_tid_address(args[0]);
        case SYS_CLOCK_GETTIME:   return sys_clock_gettime(args[0], args[1]);
        case SYS_CLOCK_NANOSLEEP: return sys_clock_nanosleep(args[0], args[1], args[2], args[3]);
        case SYS_EXIT_GROUP:      return sys_exit_group(args[0]);
        case SYS_TGKILL:          return sys_tgkill(args[0], args[1], args[2]);
        case SYS_OPENAT:          return sys_openat(args[0], args[1], args[2], args[3]);
        case SYS_MKDIRAT:         return sys_mkdirat(args[0], args[1], args[2]);
        case SYS_NEWFSTATAT:      return sys_newfstatat(args[0], args[1], args[2], args[3]);
        case SYS_READLINKAT:      return sys_readlinkat(args[0], args[1], args[2], args[3]);
        case SYS_DUP3:            return sys_dup3(args[0], args[1], args[2]);
        case SYS_PIPE2:           return sys_pipe2(args[0], args[1]);
        case SYS_PRLIMIT64:       return sys_prlimit64(args[0], args[1], args[2], args[3]);
        case SYS_FACCESSAT:       return sys_faccessat(args[0], args[1], args[2]);
        case SYS_FACCESSAT2:      return sys_faccessat(args[0], args[1], args[2]); /* flags: no users to tell apart */
    }

    printk("syscall: %ld not implemented -- task %lu\n", nr, current_task()->id);
    return -ENOSYS;
}
