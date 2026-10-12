#include <serika/boot.h>
#include <serika/buddy.h>
#include <serika/cpu.h>
#include <serika/errno.h>
#include <serika/mm.h>
#include <serika/printk.h>
#include <serika/string.h>
#include <serika/syscall.h>
#include <serika/sched.h>
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

/* nothing is limited, the stack reports the usual 8 MiB. a limit can be set, and is not kept: it is taken and nothing
 * is changed (programs lower their file limit for themselves, and take a refusal for a failure) */
long sys_prlimit64(long pid, long resource, long unew, long uold) {
    if (pid != 0 && pid != sys_getpid()) return -ESRCH;

    if (unew) {
        struct { uint64_t cur, max; } rl;

        if (copy_from_user(&rl, unew, sizeof(rl)) != 0) return -EFAULT;
        if (rl.cur > rl.max) return -EINVAL;
    }

    if (uold) {
        struct { uint64_t cur, max; } rl = { RLIM_INFINITY, RLIM_INFINITY };

        if (resource == RLIMIT_STACK) rl.cur = 8 << 20;

        return copy_to_user(uold, &rl, sizeof(rl));
    }

    return 0;
}

/* capabilities: there are no users, so there is only root, who has them all */
#define LINUX_CAPABILITY_VERSION_1 0x19980330
#define LINUX_CAPABILITY_VERSION_2 0x20071026
#define LINUX_CAPABILITY_VERSION_3 0x20080522

struct cap_header {
    uint32_t version;
    int32_t  pid;
};

struct cap_data {
    uint32_t effective;
    uint32_t permitted;
    uint32_t inheritable;
};

long sys_capget(long uhdr, long udata) {
    struct cap_header h;

    if (copy_from_user(&h, uhdr, sizeof(h)) != 0) return -EFAULT;

    if (h.version != LINUX_CAPABILITY_VERSION_1 && h.version != LINUX_CAPABILITY_VERSION_2 &&
        h.version != LINUX_CAPABILITY_VERSION_3) {
        h.version = LINUX_CAPABILITY_VERSION_3; /* told back, with the error */
        copy_to_user(uhdr, &h, sizeof(h));
        return -EINVAL;
    }

    if (!udata) return 0; /* just the version */

    struct cap_data d[2] = { { ~0u, ~0u, 0 }, { ~0u, ~0u, 0 } };

    return copy_to_user(udata, d, (h.version == LINUX_CAPABILITY_VERSION_1 ? 1 : 2) * sizeof(d[0]));
}

/* taken and not kept: it cannot lose what it has, and has everything */
long sys_capset(long uhdr, long udata) {
    (void)uhdr; (void)udata;
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

/* the mask is the online cpus: bit n is cpu n. its size is a whole number of longs, as linux's */
long sys_sched_getaffinity(long pid, long size, long umask) {
    uint64_t online = g_bootinfo.smp.total_cores >= 64 ? ~0ULL : (1ULL << g_bootinfo.smp.total_cores) - 1;

    if (size < 0 || (size & 7)) return -EINVAL;
    if (size < 8) return -EINVAL;

    if (pid != 0 && sys_getpgid(pid) < 0) return -ESRCH;

    /* bits past the 64th would be more cpus than the mask above names: not here */
    uint64_t mask[16] = { online };
    size_t bytes = size > (long)sizeof(mask) ? sizeof(mask) : (size_t)size;

    return copy_to_user(umask, mask, bytes) ?: (long)bytes;
}

/* tasks run on any cpu the scheduler picks: a mask with an online cpu in it is accepted, and not kept */
long sys_sched_setaffinity(long pid, long size, long umask) {
    uint64_t first;

    if (size < 8) return -EINVAL;
    if (pid != 0 && sys_getpgid(pid) < 0) return -ESRCH;

    if (copy_from_user(&first, umask, sizeof(first)) != 0) return -EFAULT;

    uint64_t online = g_bootinfo.smp.total_cores >= 64 ? ~0ULL : (1ULL << g_bootinfo.smp.total_cores) - 1;

    return (first & online) ? 0 : -EINVAL;
}

#define PRIO_PROCESS 0

/* only a thread, and only by number: groups and users are not here. nice is -20 to 19 */
long sys_setpriority(long which, long who, long prio) {
    int nice = prio < -20 ? -20 : prio > 19 ? 19 : (int)prio;

    if (which != PRIO_PROCESS) return -EINVAL;

    return task_nice(who, &nice, true);
}

/* the raw call gives 20 - nice, so that it is never negative */
long sys_getpriority(long which, long who) {
    int nice;

    if (which != PRIO_PROCESS) return -EINVAL;

    long err = task_nice(who, &nice, false);
    return err ? err : 20 - nice;
}

#define SCHED_OTHER 0
#define SCHED_BATCH 3
#define SCHED_IDLE  5

/* the fair policies are all there is: batch and idle are what other is. realtime is refused */
long sys_sched_setscheduler(long pid, long policy, long uparam) {
    int32_t priority;

    if (copy_from_user(&priority, uparam, sizeof(priority)) != 0) return -EFAULT;
    if (policy != SCHED_OTHER && policy != SCHED_BATCH && policy != SCHED_IDLE) return policy < 0 || policy > 5 ? -EINVAL : -EPERM;
    if (priority != 0) return -EINVAL;

    int nice;
    return task_nice(pid, &nice, false);
}

long sys_sched_getscheduler(long pid) {
    int nice;
    long err = task_nice(pid, &nice, false);

    return err ? err : SCHED_OTHER;
}

long sys_sched_getparam(long pid, long uparam) {
    int nice, priority = 0;
    long err = task_nice(pid, &nice, false);

    return err ? err : copy_to_user(uparam, &priority, sizeof(priority));
}

/* 0 and 0 for the fair ones, as linux. the realtime range is named and cannot be used */
long sys_sched_get_priority(long policy, bool max) {
    switch (policy) {
        case SCHED_OTHER:
        case SCHED_BATCH:
        case SCHED_IDLE:
            return 0;

        case 1: /* FIFO */
        case 2: /* RR */
            return max ? 99 : 1;
    }

    return -EINVAL;
}

long sys_sched_yield(void) {
    schedule();
    return 0;
}

long sys_getcpu(long ucpu, long unode) {
    uint32_t cpu = curcpu->id, node = 0;

    if (ucpu && copy_to_user(ucpu, &cpu, sizeof(cpu)) != 0) return -EFAULT;
    if (unode && copy_to_user(unode, &node, sizeof(node)) != 0) return -EFAULT;

    return 0;
}

#define PR_SET_PDEATHSIG 1
#define PR_GET_DUMPABLE  3
#define PR_SET_DUMPABLE  4
#define PR_SET_NAME      15
#define PR_GET_NAME      16
#define PR_SET_NO_NEW_PRIVS 38
#define PR_GET_NO_NEW_PRIVS 39

/* the name of a thread, and the flags a program sets about itself, which change nothing here */
long sys_prctl(long option, long arg2) {
    struct task *curr = current_task();
    char name[16];

    switch (option) {
        case PR_SET_NAME:
            memset(name, 0, sizeof(name));
            if (strncpy_from_user(name, arg2, sizeof(name) - 1) < 0) return -EFAULT;

            memcpy(curr->comm, name, sizeof(name));
            return 0;

        case PR_GET_NAME:
            return copy_to_user(arg2, curr->comm, sizeof(curr->comm));

        case PR_GET_DUMPABLE:
            return 1;

        case PR_SET_DUMPABLE:
        case PR_SET_PDEATHSIG:
        case PR_SET_NO_NEW_PRIVS:
            return 0;

        case PR_GET_NO_NEW_PRIVS:
            return 0;
    }

    return -EINVAL;
}
