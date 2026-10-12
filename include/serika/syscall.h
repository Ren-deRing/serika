#pragma once

#define SYS_READ              0
#define SYS_WRITE             1
#define SYS_OPEN              2
#define SYS_CLOSE             3
#define SYS_STAT              4
#define SYS_FSTAT             5
#define SYS_LSTAT             6
#define SYS_POLL              7
#define SYS_LSEEK             8
#define SYS_MMAP              9
#define SYS_MPROTECT         10
#define SYS_MUNMAP           11
#define SYS_BRK              12
#define SYS_RT_SIGACTION     13
#define SYS_RT_SIGPROCMASK   14
#define SYS_RT_SIGRETURN     15
#define SYS_IOCTL            16
#define SYS_PREAD64          17
#define SYS_READV            19
#define SYS_WRITEV           20
#define SYS_ACCESS           21
#define SYS_PIPE             22
#define SYS_SELECT           23
#define SYS_MADVISE          28
#define SYS_DUP              32
#define SYS_DUP2             33
#define SYS_PAUSE            34
#define SYS_NANOSLEEP        35
#define SYS_GETPID           39
#define SYS_SENDFILE         40
#define SYS_CLONE            56
#define SYS_FORK             57
#define SYS_VFORK            58
#define SYS_EXECVE           59
#define SYS_EXIT             60
#define SYS_WAIT4            61
#define SYS_KILL             62
#define SYS_UNAME            63
#define SYS_FCNTL            72
#define SYS_GETCWD           79
#define SYS_CHDIR            80
#define SYS_MKDIR            83
#define SYS_SYMLINK          88
#define SYS_READLINK         89
#define SYS_UMASK            95
#define SYS_GETUID          102
#define SYS_GETGID          104
#define SYS_SETUID          105
#define SYS_SETGID          106
#define SYS_GETEUID         107
#define SYS_GETEGID         108
#define SYS_SETPGID         109
#define SYS_GETPPID         110
#define SYS_GETPGRP         111
#define SYS_SETSID          112
#define SYS_GETPGID         121
#define SYS_GETSID          124
#define SYS_RT_SIGPENDING   127
#define SYS_RT_SIGSUSPEND   130
#define SYS_ARCH_PRCTL      158
#define SYS_GETTID          186
#define SYS_TKILL           200
#define SYS_FUTEX           202
#define SYS_GETDENTS64      217
#define SYS_SET_TID_ADDRESS 218
#define SYS_CLOCK_GETTIME   228
#define SYS_CLOCK_NANOSLEEP 230
#define SYS_EXIT_GROUP      231
#define SYS_TGKILL          234
#define SYS_OPENAT          257
#define SYS_MKDIRAT         258
#define SYS_NEWFSTATAT      262
#define SYS_SYMLINKAT       266
#define SYS_READLINKAT      267
#define SYS_FACCESSAT       269
#define SYS_PSELECT6        270
#define SYS_PPOLL           271
#define SYS_SET_ROBUST_LIST 273
#define SYS_DUP3            292
#define SYS_PIPE2           293
#define SYS_PRLIMIT64       302
#define SYS_GETRANDOM       318
#define SYS_RSEQ            334
#define SYS_CLONE3          435
#define SYS_FACCESSAT2      439

#define WNOHANG 1

#define WEXITSTATUS(status) (((status) >> 8) & 0xff)

long syscall_dispatch(long nr, const long args[6]);
long sys_arch_prctl(long code, long addr); /* arch */

/* fs/file.c */
long sys_openat(long dirfd, long upath, long flags, long mode);
long sys_close(long fd);
long sys_read(long fd, long buf, long count);
long sys_write(long fd, long buf, long count);
long sys_lseek(long fd, long off, long whence);
long sys_fstat(long fd, long ubuf);
long sys_newfstatat(long dirfd, long upath, long ubuf, long flags);
long sys_ioctl(long fd, long cmd, long arg);
long sys_pread64(long fd, long buf, long count, long off);
long sys_access(long upath, long mode);
long sys_faccessat(long dirfd, long upath, long mode);
long sys_readv(long fd, long uiov, long count);
long sys_writev(long fd, long uiov, long count);
long sys_sendfile(long out_fd, long in_fd, long uoff, long count);
long sys_pipe2(long ufds, long flags);
long sys_dup(long fd);
long sys_dup2(long oldfd, long newfd);
long sys_dup3(long oldfd, long newfd, long flags);
long sys_fcntl(long fd, long cmd, long arg);
long sys_getdents64(long fd, long ubuf, long count);
long sys_getcwd(long ubuf, long size);
long sys_chdir(long upath);
long sys_mkdirat(long dirfd, long upath, long mode);
long sys_umask(long mask);
long sys_readlinkat(long dirfd, long upath, long ubuf, long size);
long sys_symlinkat(long utarget, long dirfd, long ulink);

/* kernel/random.c */
long sys_getrandom(long ubuf, long len, long flags);

/* kernel/sys.c */
long sys_getuid(void);
long sys_setuid(long id);
long sys_uname(long ubuf);
long sys_prlimit64(long pid, long resource, long unew, long uold);

/* fs/select.c */
long sys_poll(long ufds, long nfds, long timeout_ms);
long sys_ppoll(long ufds, long nfds, long utsp, long usigmask, long sigsetsize);
long sys_select(long n, long ur, long uw, long ue, long utv);
long sys_pselect6(long n, long ur, long uw, long ue, long utsp, long usig);

/* kernel/time.c */
long sys_clock_gettime(long clk, long utp);
long sys_nanosleep(long ureq, long urem);
long sys_clock_nanosleep(long clk, long flags, long ureq, long urem);

/* kernel/signal.c */
long sys_rt_sigaction(long sig, long act, long oact, long sigsetsize);
long sys_rt_sigprocmask(long how, long set, long oset, long sigsetsize);
long sys_rt_sigpending(long uset, long sigsetsize);
long sys_pause(void);
long sys_rt_sigsuspend(long uset, long size);
long sys_kill(long pid, long sig);
long sys_tkill(long tid, long sig);
long sys_tgkill(long tgid, long tid, long sig);
long sys_getpid(void);
long sys_gettid(void);

/* kernel/fork.c */
long sys_clone(long flags, long newsp, long ptid, long ctid, long tls);
long sys_clone3(long uargs, long size);
long sys_fork(void);
long sys_vfork(void);
long sys_wait4(long pid, long ustatus, long options, long urusage);

/* kernel/sched/task.c */
long sys_getppid(void);
long sys_getpgrp(void);
long sys_getpgid(long pid);
long sys_setpgid(long pid, long pgid);
long sys_setsid(void);
long sys_getsid(long pid);

/* fs/exec.c */
long sys_execve(long upath, long uargv, long uenvp);

/* kernel/futex.c */
long sys_futex(long uaddr, long op, long val, long utime, long uaddr2, long val3);
long sys_set_robust_list(long head, long len);
