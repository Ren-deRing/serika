#include <serika/buddy.h>
#include <serika/cpu.h>
#include <serika/errno.h>
#include <serika/mm.h>
#include <serika/printk.h>
#include <serika/string.h>
#include <serika/syscall.h>
#include <serika/task.h>

#include <asm/arch.h>

#define RLIMIT_STACK 3
#define RLIM_INFINITY (~0ULL)

/* no users: everything is root, and it stays root */
long sys_getuid(void) {
    return 0;
}

long sys_setuid(long id) {
    return id == 0 ? 0 : -EPERM;
}

long sys_uname(long ubuf) {
    struct { char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65]; } u;

    memset(&u, 0, sizeof(u));
    strcpy(u.sysname, "serika");
    strcpy(u.nodename, "serika");
    strcpy(u.release, "0.0.1");
    strcpy(u.version, "serika");
    strcpy(u.machine, "x86_64");
    strcpy(u.domainname, "(none)");

    return copy_to_user(ubuf, &u, sizeof(u));
}

/* nothing is limited, the stack reports the usual 8 MiB. none can be set */
long sys_prlimit64(long pid, long resource, long unew, long uold) {
    if (pid != 0 && pid != sys_getpid()) return -ESRCH;
    if (unew) return -EPERM;

    if (uold) {
        struct { uint64_t cur, max; } rl = { RLIM_INFINITY, RLIM_INFINITY };

        if (resource == RLIMIT_STACK) rl.cur = 8 << 20;

        return copy_to_user(uold, &rl, sizeof(rl));
    }

    return 0;
}

struct sysinfo {
    int64_t  uptime;
    uint64_t loads[3];
    uint64_t totalram;
    uint64_t freeram;
    uint64_t sharedram;
    uint64_t bufferram;
    uint64_t totalswap;
    uint64_t freeswap;
    uint16_t procs;
    uint16_t pad;
    uint32_t pad2;
    uint64_t totalhigh;
    uint64_t freehigh;
    uint32_t mem_unit;
    uint32_t pad3;
};

/* memory is the buddy's. nothing is cached or shared or swapped, and no load is kept */
long sys_sysinfo(long ubuf) {
    struct sysinfo si = { 0 };

    si.uptime   = rdtsc() / this_cpu_read(tsc_freq_hz);
    si.totalram = buddy_managed_pages();
    si.freeram  = buddy_free_pages();
    si.procs    = 1;
    si.mem_unit = PAGE_SIZE;

    return copy_to_user(ubuf, &si, sizeof(si));
}

#define RUSAGE_CHILDREN (-1)
#define RUSAGE_SELF     0
#define RUSAGE_THREAD   1

/* no accounting yet: all zero, as wait4 gives */
long sys_getrusage(long who, long ubuf) {
    uint8_t zero[144] = { 0 };

    if (who != RUSAGE_SELF && who != RUSAGE_CHILDREN && who != RUSAGE_THREAD) return -EINVAL;

    return copy_to_user(ubuf, zero, sizeof(zero));
}
