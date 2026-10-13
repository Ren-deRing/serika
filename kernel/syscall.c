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
/* private: anonymous, or a file read in page by page as it is touched. shared: the pages of a file, written in
 * place, or anonymous memory that is a file nobody can open, as linux does it */
static long sys_mmap(long addr, long len, long prot, long flags, long fd, long off) {
    struct file *file = NULL;
    bool shared = (flags & (MAP_SHARED | MAP_PRIVATE)) == MAP_SHARED;

    if (!shared && (flags & (MAP_SHARED | MAP_PRIVATE)) != MAP_PRIVATE) {
        printk("syscall: mmap flags 0x%lx not supported -- task %lu\n", flags, current_task()->id);
        return -EINVAL;
    }

    if (len <= 0) return -EINVAL;

    if (flags & MAP_ANONYMOUS) {
        if (shared) {
            struct inode *inode = ramfs_anon_file();
            if (inode) {
                inode->size = ALIGN_UP(len, PAGE_SIZE);
                file = file_alloc(inode, O_RDWR);
                if (!file) iput(inode);
            }

            if (!file) return -ENOMEM;
        }
    } else {
        if (!(file = fd_get(fd))) return -EBADF;

        bool rdonly = (file->flags & O_ACCMODE) == O_RDONLY, wronly = (file->flags & O_ACCMODE) == O_WRONLY;

        bool mappable = S_ISREG(file->inode->mode) || (S_ISCHR(file->inode->mode) && shared && file->inode->ops->getpage);

        if (!mappable || wronly || (file->flags & O_PATH) ||
            (shared && (!file->inode->ops->getpage || ((prot & PROT_WRITE) && rdonly)))) {
            vfs_close(file);
            return -EACCES;
        }
    }

    long ret = mm_mmap(current_task()->mm, addr, len, prot, flags, file, file && !(flags & MAP_ANONYMOUS) ? off : 0);

    if (file) vfs_close(file);
    return ret;
}


#define MADV_DONTNEED 4
#define MADV_FREE     8
#define MADV_GUARD_INSTALL 102
#define MADV_GUARD_REMOVE  103

/* DONTNEED and FREE drop the pages, and the memory reads zero after. what only advises is taken and changes
 * nothing. what changes what fork or a file does is refused: DONTFORK, WIPEONFORK, REMOVE */
static long sys_madvise(long addr, long len, long advice) {
    switch (advice) {
        case MADV_DONTNEED:
        case MADV_FREE:
            return mm_discard(current_task()->mm, addr, len);

        /* a range nothing may touch, and given back: glibc's stack guards. not a marker inside a vma, a protection */
        case MADV_GUARD_INSTALL:
            return mm_protect(current_task()->mm, addr, len, PROT_NONE);

        case MADV_GUARD_REMOVE:
            return mm_protect(current_task()->mm, addr, len, PROT_READ | PROT_WRITE);

        case 0:  /* NORMAL */
        case 1:  /* RANDOM */
        case 2:  /* SEQUENTIAL */
        case 3:  /* WILLNEED */
        case 12: /* MERGEABLE */
        case 13: /* UNMERGEABLE */
        case 14: /* HUGEPAGE */
        case 15: /* NOHUGEPAGE */
        case 16: /* DONTDUMP */
        case 17: /* DODUMP */
        case 20: /* COLD */
        case 21: /* PAGEOUT */
            return 0;
    }

    return -EINVAL;
}

#define MREMAP_MAYMOVE 1

/* the mapping stays where it is unless MAYMOVE. FIXED and DONTUNMAP are not there */
static long sys_mremap(long old, long old_size, long new_size, long flags) {
    if (flags & ~MREMAP_MAYMOVE) return -EINVAL;

    return mm_remap(current_task()->mm, old, old_size, new_size, flags & MREMAP_MAYMOVE);
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

static long do_syscall(long nr, const long args[6]);

volatile int syscall_trace; /* /dev/strace: every call, its arguments and what it returned, on the console */

static const char *syscall_name(long nr) {
    switch (nr) {
        case SYS_READ: return "read";             case SYS_WRITE: return "write";
        case SYS_OPEN: return "open";             case SYS_CLOSE: return "close";
        case SYS_STAT: return "stat";             case SYS_FSTAT: return "fstat";
        case SYS_LSTAT: return "lstat";           case SYS_POLL: return "poll";
        case SYS_LSEEK: return "lseek";           case SYS_MMAP: return "mmap";
        case SYS_MPROTECT: return "mprotect";     case SYS_MUNMAP: return "munmap";
        case SYS_IOCTL: return "ioctl";           case SYS_PREAD64: return "pread64";
        case SYS_PWRITE64: return "pwrite64";
        case SYS_ACCESS: return "access";         case SYS_PIPE: return "pipe";
        case SYS_DUP: return "dup";               case SYS_DUP2: return "dup2";
        case SYS_SOCKET: return "socket";         case SYS_CONNECT: return "connect";
        case SYS_BIND: return "bind";             case SYS_LISTEN: return "listen";
        case SYS_SETSOCKOPT: return "setsockopt"; case SYS_GETSOCKOPT: return "getsockopt";
        case SYS_GETSOCKNAME: return "getsockname"; case SYS_SENDMSG: return "sendmsg";
        case SYS_RECVMSG: return "recvmsg";       case SYS_CLONE: return "clone";
        case SYS_FORK: return "fork";             case SYS_VFORK: return "vfork";
        case SYS_EXECVE: return "execve";         case SYS_EXIT: return "exit";
        case SYS_WAIT4: return "wait4";           case SYS_KILL: return "kill";
        case SYS_FCNTL: return "fcntl";           case SYS_GETCWD: return "getcwd";
        case SYS_READLINK: return "readlink";     case SYS_FSTATFS: return "fstatfs";
        case SYS_STATFS: return "statfs";         case SYS_GETDENTS64: return "getdents64";
        case SYS_OPENAT: return "openat";         case SYS_NEWFSTATAT: return "newfstatat";
        case SYS_READLINKAT: return "readlinkat"; case SYS_FACCESSAT: return "faccessat";
        case SYS_FACCESSAT2: return "faccessat2"; case SYS_STATX: return "statx";
        case SYS_PRLIMIT64: return "prlimit64";   case SYS_CAPGET: return "capget";
        case SYS_WAITID: return "waitid";         case SYS_EPOLL_WAIT: return "epoll_wait";
        case SYS_EPOLL_CTL: return "epoll_ctl";   case SYS_EPOLL_CREATE1: return "epoll_create1";
        case SYS_FLOCK: return "flock";           case SYS_PPOLL: return "ppoll";
        case SYS_UNLINKAT: return "unlinkat";     case SYS_MKDIRAT: return "mkdirat";
        case SYS_GETRANDOM: return "getrandom";   case SYS_MEMFD_CREATE: return "memfd_create";
        case SYS_BRK: return "brk";               case SYS_FUTEX: return "futex";
    }

    return "?";
}

/* the argument that is a path, if the call has one */
static int syscall_path_arg(long nr) {
    switch (nr) {
        case SYS_OPEN: case SYS_STAT: case SYS_LSTAT: case SYS_ACCESS: case SYS_READLINK: case SYS_STATFS:
        case SYS_EXECVE:
            return 0;

        case SYS_OPENAT: case SYS_NEWFSTATAT: case SYS_READLINKAT: case SYS_FACCESSAT: case SYS_FACCESSAT2:
        case SYS_STATX: case SYS_UNLINKAT: case SYS_MKDIRAT:
            return 1;
    }

    return -1;
}

long syscall_dispatch(long nr, const long args[6]) {
    if (!syscall_trace) return do_syscall(nr, args);

    char path[64] = "";
    int pa = syscall_path_arg(nr);

    if (pa >= 0) {
        long n = strncpy_from_user(path, args[pa], sizeof(path) - 1);
        path[n > 0 ? n : 0] = 0;
    }

    long ret = do_syscall(nr, args);

    printk("trace %lu: %s(%ld)(%lx, %lx, %lx, %lx)%s%s%s = %ld\n", current_task()->id, syscall_name(nr), nr, args[0],
           args[1], args[2], args[3], path[0] ? " \"" : "", path, path[0] ? "\"" : "", ret);
    return ret;
}

/* an int or pid_t argument has its low 32 bits only: the rest of the register is whatever the caller left */
static long do_syscall(long nr, const long args[6]) {
    switch (nr) {
        case SYS_READ:            return sys_read(args[0], args[1], args[2]);
        case SYS_WRITE:           return sys_write(args[0], args[1], args[2]);
        case SYS_OPEN:            return sys_openat(AT_FDCWD, args[0], args[1], args[2]);
        case SYS_CLOSE:           return sys_close(args[0]);
        case SYS_CLOSE_RANGE:     return sys_close_range((uint32_t)args[0], (uint32_t)args[1], (uint32_t)args[2]);
        case SYS_STAT:            return sys_newfstatat(AT_FDCWD, args[0], args[1], 0);
        case SYS_FSTAT:           return sys_fstat(args[0], args[1]);
        case SYS_LSTAT:           return sys_newfstatat(AT_FDCWD, args[0], args[1], AT_SYMLINK_NOFOLLOW);
        case SYS_LSEEK:           return sys_lseek(args[0], args[1], args[2]);
        case SYS_MMAP:            return sys_mmap(args[0], args[1], args[2], args[3], args[4], args[5]);
        case SYS_MPROTECT:        return sys_mprotect(args[0], args[1], args[2]);
        case SYS_MUNMAP:          return sys_munmap(args[0], args[1]);
        case SYS_MREMAP:          return sys_mremap(args[0], args[1], args[2], (int)args[3]);
        case SYS_MADVISE:         return sys_madvise(args[0], args[1], args[2]);

        /* there is no swap: what is mapped is in memory, so it is locked in it already */
        case SYS_MLOCK:
        case SYS_MLOCK2:
        case SYS_MUNLOCK:
        case SYS_MLOCKALL:
        case SYS_MUNLOCKALL:      return 0;

        case SYS_BRK:             return sys_brk(args[0]);
        case SYS_RT_SIGACTION:    return sys_rt_sigaction(args[0], args[1], args[2], args[3]);
        case SYS_RT_SIGPROCMASK:  return sys_rt_sigprocmask(args[0], args[1], args[2], args[3]);
        case SYS_IOCTL:           return sys_ioctl(args[0], (uint32_t)args[1], args[2]);
        case SYS_PREAD64:         return sys_pread64(args[0], args[1], args[2], args[3]);
        case SYS_PWRITE64:        return sys_pwrite64(args[0], args[1], args[2], args[3]);
        case SYS_READV:           return sys_readv(args[0], args[1], args[2]);
        case SYS_WRITEV:          return sys_writev(args[0], args[1], args[2]);
        case SYS_POLL:            return sys_poll(args[0], args[1], (int)args[2]);
        case SYS_SELECT:          return sys_select((int)args[0], args[1], args[2], args[3], args[4]);
        case SYS_ACCESS:          return sys_access(args[0], args[1]);
        case SYS_PIPE:            return sys_pipe2(args[0], 0);
        case SYS_DUP:             return sys_dup(args[0]);
        case SYS_DUP2:            return sys_dup2(args[0], args[1]);
        case SYS_SOCKET:          return sys_socket((int)args[0], (int)args[1], (int)args[2]);
        case SYS_CONNECT:         return sys_connect((int)args[0], args[1], (int)args[2]);
        case SYS_ACCEPT:          return sys_accept((int)args[0], args[1], args[2]);
        case SYS_SENDTO:          return sys_sendto((int)args[0], args[1], args[2], (int)args[3], args[4], (int)args[5]);
        case SYS_RECVFROM:        return sys_recvfrom((int)args[0], args[1], args[2], (int)args[3], args[4], args[5]);
        case SYS_SENDMSG:         return sys_sendmsg((int)args[0], args[1], (int)args[2]);
        case SYS_RECVMSG:         return sys_recvmsg((int)args[0], args[1], (int)args[2]);
        case SYS_SHUTDOWN:        return sys_shutdown((int)args[0], (int)args[1]);
        case SYS_BIND:            return sys_bind((int)args[0], args[1], (int)args[2]);
        case SYS_LISTEN:          return sys_listen((int)args[0], (int)args[1]);
        case SYS_GETSOCKNAME:     return sys_getsockname((int)args[0], args[1], args[2]);
        case SYS_GETPEERNAME:     return sys_getpeername((int)args[0], args[1], args[2]);
        case SYS_SOCKETPAIR:      return sys_socketpair((int)args[0], (int)args[1], (int)args[2], args[3]);
        case SYS_SETSOCKOPT:      return sys_setsockopt((int)args[0], (int)args[1], (int)args[2], args[3], (int)args[4]);
        case SYS_GETSOCKOPT:      return sys_getsockopt((int)args[0], (int)args[1], (int)args[2], args[3], args[4]);
        case SYS_ACCEPT4:         return sys_accept4((int)args[0], args[1], args[2], (int)args[3]);
        case SYS_RECVMMSG:        return sys_recvmmsg((int)args[0], args[1], (int)args[2], (int)args[3], args[4]);
        case SYS_SENDMMSG:        return sys_sendmmsg((int)args[0], args[1], (int)args[2], (int)args[3]);
        case SYS_NANOSLEEP:       return sys_nanosleep(args[0], args[1]);
        case SYS_SENDFILE:        return sys_sendfile(args[0], args[1], args[2], args[3]);
        case SYS_PAUSE:           return sys_pause();
        case SYS_GETPID:          return sys_getpid();
        case SYS_CLONE:           return sys_clone(args[0], args[1], args[2], args[3], args[4]);
        case SYS_FORK:            return sys_fork();
        case SYS_VFORK:           return sys_vfork();
        case SYS_EXECVE:          return sys_execve(args[0], args[1], args[2]);
        case SYS_EXIT:            return sys_exit(args[0]);
        case SYS_WAIT4:           return sys_wait4((int)args[0], args[1], args[2], args[3]);
        case SYS_KILL:            return sys_kill((int)args[0], (int)args[1]);
        case SYS_UNAME:           return sys_uname(args[0]);
        case SYS_FCNTL:           return sys_fcntl(args[0], (int)args[1], args[2]);
        case SYS_GETCWD:          return sys_getcwd(args[0], args[1]);
        case SYS_CHDIR:           return sys_chdir(args[0]);
        case SYS_TRUNCATE:        return sys_truncate(args[0], args[1]);
        case SYS_FTRUNCATE:       return sys_ftruncate(args[0], args[1]);
        case SYS_RENAME:          return sys_renameat2(AT_FDCWD, args[0], AT_FDCWD, args[1], 0);
        case SYS_RMDIR:           return sys_unlinkat(AT_FDCWD, args[0], AT_REMOVEDIR);
        case SYS_LINK:            return sys_linkat(AT_FDCWD, args[0], AT_FDCWD, args[1], 0);
        case SYS_UNLINK:          return sys_unlinkat(AT_FDCWD, args[0], 0);
        case SYS_CHMOD:           return sys_fchmodat(AT_FDCWD, args[0], args[1]);
        case SYS_FCHMOD:          return sys_fchmod(args[0], args[1]);
        case SYS_CHOWN:
        case SYS_LCHOWN:          return sys_fchownat(AT_FDCWD, args[0], (int)args[1], (int)args[2], 0);
        case SYS_FCHOWN:          return sys_fchownat(args[0], 0, (int)args[1], (int)args[2], 0);
        case SYS_FSYNC:
        case SYS_FDATASYNC:       return sys_fsync(args[0]);
        case SYS_SYNC:            return sys_sync();
        case SYS_MKNOD:           return sys_mknodat(AT_FDCWD, args[0], args[1], args[2]);
        case SYS_MKDIR:           return sys_mkdirat(AT_FDCWD, args[0], args[1]);
        case SYS_SYMLINK:         return sys_symlinkat(args[0], AT_FDCWD, args[1]);
        case SYS_READLINK:        return sys_readlinkat(AT_FDCWD, args[0], args[1], args[2]);
        case SYS_GETTIMEOFDAY:    return sys_gettimeofday(args[0], args[1]);
        case SYS_TIME:            return sys_time(args[0]);
        case SYS_SCHED_YIELD:     return sys_sched_yield();
        case SYS_MSYNC:           return 0; /* memory only: nothing to write out */
        case SYS_GETPRIORITY:     return sys_getpriority((int)args[0], (int)args[1]);
        case SYS_SETPRIORITY:     return sys_setpriority((int)args[0], (int)args[1], (int)args[2]);
        case SYS_SCHED_GETPARAM:  return sys_sched_getparam((int)args[0], args[1]);
        case SYS_SCHED_SETSCHEDULER: return sys_sched_setscheduler((int)args[0], (int)args[1], args[2]);
        case SYS_SCHED_GETSCHEDULER: return sys_sched_getscheduler((int)args[0]);
        case SYS_SCHED_GET_PRIORITY_MAX: return sys_sched_get_priority((int)args[0], true);
        case SYS_SCHED_GET_PRIORITY_MIN: return sys_sched_get_priority((int)args[0], false);
        case SYS_PRCTL:           return sys_prctl((int)args[0], args[1]);
        case SYS_GETCPU:          return sys_getcpu(args[0], args[1]);
        case SYS_FALLOCATE:       return sys_fallocate(args[0], (int)args[1], args[2], args[3]);
        case SYS_MEMFD_CREATE:    return sys_memfd_create(args[0], (uint32_t)args[1]);
        case SYS_GETRUSAGE:       return sys_getrusage((int)args[0], args[1]);
        case SYS_SYSINFO:         return sys_sysinfo(args[0]);
        case SYS_UMASK:           return sys_umask(args[0]);
        case SYS_GETUID:
        case SYS_GETGID:
        case SYS_GETEUID:
        case SYS_GETEGID:         return sys_getuid();
        case SYS_SETUID:
        case SYS_SETGID:          return sys_setuid(args[0]);
        case SYS_GETRESUID:
        case SYS_GETRESGID:       return sys_getresuid(args[0], args[1], args[2]);
        case SYS_GETPPID:         return sys_getppid();
        case SYS_SETPGID:         return sys_setpgid((int)args[0], (int)args[1]);
        case SYS_GETPGRP:         return sys_getpgrp();
        case SYS_SETSID:          return sys_setsid();
        case SYS_GETPGID:         return sys_getpgid((int)args[0]);
        case SYS_GETSID:          return sys_getsid((int)args[0]);
        case SYS_SCHED_SETAFFINITY: return sys_sched_setaffinity((int)args[0], args[1], args[2]);
        case SYS_SCHED_GETAFFINITY: return sys_sched_getaffinity((int)args[0], args[1], args[2]);
        case SYS_RT_SIGPENDING:   return sys_rt_sigpending(args[0], args[1]);
        case SYS_RT_SIGSUSPEND:   return sys_rt_sigsuspend(args[0], args[1]);
        case SYS_ARCH_PRCTL:      return sys_arch_prctl(args[0], args[1]);
        case SYS_GETTID:          return sys_gettid();
        case SYS_TKILL:           return sys_tkill((int)args[0], (int)args[1]);
        case SYS_FUTEX:           return sys_futex(args[0], (int)args[1], (int)args[2], args[3], args[4], (uint32_t)args[5]);
        case SYS_GETDENTS64:      return sys_getdents64(args[0], args[1], args[2]);
        case SYS_SET_ROBUST_LIST: return sys_set_robust_list(args[0], args[1]);
        case SYS_SET_TID_ADDRESS: return sys_set_tid_address(args[0]);
        case SYS_CLOCK_GETTIME:   return sys_clock_gettime(args[0], args[1]);
        case SYS_CLOCK_GETRES:    return sys_clock_getres(args[0], args[1]);
        case SYS_CLOCK_NANOSLEEP: return sys_clock_nanosleep(args[0], args[1], args[2], args[3]);
        case SYS_EXIT_GROUP:      return sys_exit_group(args[0]);
        case SYS_TGKILL:          return sys_tgkill((int)args[0], (int)args[1], (int)args[2]);
        case SYS_OPENAT:          return sys_openat((int)args[0], args[1], args[2], args[3]);
        case SYS_FCHOWNAT:        return sys_fchownat((int)args[0], args[1], (int)args[2], (int)args[3], (int)args[4]);
        case SYS_UNLINKAT:        return sys_unlinkat((int)args[0], args[1], (int)args[2]);
        case SYS_RENAMEAT:        return sys_renameat2((int)args[0], args[1], (int)args[2], args[3], 0);
        case SYS_LINKAT:          return sys_linkat((int)args[0], args[1], (int)args[2], args[3], (int)args[4]);
        case SYS_FCHMODAT:        return sys_fchmodat((int)args[0], args[1], args[2]);
        case SYS_UTIMENSAT:       return sys_utimensat((int)args[0], args[1], args[2], (int)args[3]);
        case SYS_RENAMEAT2:       return sys_renameat2((int)args[0], args[1], (int)args[2], args[3], (int)args[4]);
        case SYS_MKNODAT:         return sys_mknodat((int)args[0], args[1], args[2], args[3]);
        case SYS_MKDIRAT:         return sys_mkdirat((int)args[0], args[1], args[2]);
        case SYS_NEWFSTATAT:      return sys_newfstatat((int)args[0], args[1], args[2], args[3]);
        case SYS_CAPGET:          return sys_capget(args[0], args[1]);
        case SYS_CAPSET:          return sys_capset(args[0], args[1]);
        case SYS_WAITID:          return sys_waitid((int)args[0], (int)args[1], args[2], (int)args[3], args[4]);
        case SYS_STATFS:          return sys_statfs(args[0], args[1]);
        case SYS_FSTATFS:         return sys_fstatfs((int)args[0], args[1]);
        case SYS_STATX:           return sys_statx((int)args[0], args[1], (int)args[2], args[3], args[4]);
        case SYS_SYMLINKAT:       return sys_symlinkat(args[0], (int)args[1], args[2]);
        case SYS_READLINKAT:      return sys_readlinkat((int)args[0], args[1], args[2], args[3]);
        case SYS_DUP3:            return sys_dup3(args[0], args[1], args[2]);
        case SYS_PIPE2:           return sys_pipe2(args[0], args[1]);
        case SYS_GETRANDOM:       return sys_getrandom(args[0], args[1], args[2]);
        case SYS_CLONE3:          return sys_clone3(args[0], args[1]);
        case SYS_RSEQ:            return -ENOSYS; /* its users run without: glibc does */
        case SYS_PRLIMIT64:       return sys_prlimit64((int)args[0], (int)args[1], args[2], args[3]);
        case SYS_PSELECT6:        return sys_pselect6((int)args[0], args[1], args[2], args[3], args[4], args[5]);
        case SYS_PPOLL:           return sys_ppoll(args[0], args[1], args[2], args[3], args[4]);
        case SYS_MOUNT:           return sys_mount(args[0], args[1], args[2], args[3], args[4]);
        case SYS_SYNCFS:          return sys_syncfs(args[0]);
        case SYS_UMOUNT2:         return sys_umount2(args[0], args[1]);
        case SYS_PIVOT_ROOT:      return sys_pivot_root(args[0], args[1]);
        case SYS_FLOCK:           return sys_flock((int)args[0], (int)args[1]);
        case SYS_EVENTFD:         return sys_eventfd2((uint32_t)args[0], 0);
        case SYS_EVENTFD2:        return sys_eventfd2((uint32_t)args[0], (int)args[1]);
        case SYS_TIMERFD_CREATE:  return sys_timerfd_create((int)args[0], (int)args[1]);
        case SYS_TIMERFD_SETTIME: return sys_timerfd_settime((int)args[0], (int)args[1], args[2], args[3]);
        case SYS_TIMERFD_GETTIME: return sys_timerfd_gettime((int)args[0], args[1]);
        case SYS_SIGNALFD:        return sys_signalfd4((int)args[0], args[1], args[2], 0);
        case SYS_SIGNALFD4:       return sys_signalfd4((int)args[0], args[1], args[2], (int)args[3]);
        case SYS_EPOLL_CREATE:    return sys_epoll_create((int)args[0]);
        case SYS_EPOLL_CREATE1:   return sys_epoll_create1((int)args[0]);
        case SYS_EPOLL_CTL:       return sys_epoll_ctl((int)args[0], (int)args[1], (int)args[2], args[3]);
        case SYS_EPOLL_WAIT:      return sys_epoll_wait((int)args[0], args[1], (int)args[2], (int)args[3]);
        case SYS_EPOLL_PWAIT:     return sys_epoll_pwait((int)args[0], args[1], (int)args[2], (int)args[3], args[4], args[5]);
        case SYS_EPOLL_PWAIT2:    return sys_epoll_pwait2((int)args[0], args[1], (int)args[2], args[3], args[4], args[5]);
        case SYS_FACCESSAT:       return sys_faccessat((int)args[0], args[1], args[2]);
        case SYS_FACCESSAT2:      return sys_faccessat((int)args[0], args[1], args[2]); /* flags: no users to tell apart */
    }

    printk("syscall: %ld not implemented -- task %lu\n", nr, current_task()->id);
    return -ENOSYS;
}
