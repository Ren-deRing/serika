#pragma once

#define SYS_READ              0
#define SYS_WRITE             1
#define SYS_CLOSE             3
#define SYS_FSTAT             5
#define SYS_LSEEK             8
#define SYS_MMAP              9
#define SYS_MPROTECT         10
#define SYS_MUNMAP           11
#define SYS_BRK              12
#define SYS_RT_SIGACTION     13
#define SYS_RT_SIGPROCMASK   14
#define SYS_RT_SIGRETURN     15
#define SYS_IOCTL            16
#define SYS_NANOSLEEP        35
#define SYS_GETPID           39
#define SYS_EXIT             60
#define SYS_KILL             62
#define SYS_RT_SIGPENDING   127
#define SYS_ARCH_PRCTL      158
#define SYS_GETTID          186
#define SYS_TKILL           200
#define SYS_SET_TID_ADDRESS 218
#define SYS_CLOCK_GETTIME   228
#define SYS_CLOCK_NANOSLEEP 230
#define SYS_EXIT_GROUP      231
#define SYS_TGKILL          234
#define SYS_OPENAT          257
#define SYS_NEWFSTATAT      262

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

/* kernel/time.c */
long sys_clock_gettime(long clk, long utp);
long sys_nanosleep(long ureq, long urem);
long sys_clock_nanosleep(long clk, long flags, long ureq, long urem);

/* kernel/signal.c */
long sys_rt_sigaction(long sig, long act, long oact, long sigsetsize);
long sys_rt_sigprocmask(long how, long set, long oset, long sigsetsize);
long sys_rt_sigpending(long uset, long sigsetsize);
long sys_kill(long pid, long sig);
long sys_tkill(long tid, long sig);
long sys_tgkill(long tgid, long tid, long sig);
long sys_getpid(void);
long sys_gettid(void);
