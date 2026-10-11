#pragma once

#define SYS_WRITE             1
#define SYS_MMAP              9
#define SYS_MPROTECT         10
#define SYS_BRK              12
#define SYS_RT_SIGACTION     13
#define SYS_RT_SIGPROCMASK   14
#define SYS_EXIT             60
#define SYS_ARCH_PRCTL      158
#define SYS_SET_TID_ADDRESS 218
#define SYS_EXIT_GROUP      231

#define WNOHANG 1

#define WEXITSTATUS(status) (((status) >> 8) & 0xff)

long syscall_dispatch(long nr, const long args[6]);
long sys_arch_prctl(long code, long addr); /* arch */
