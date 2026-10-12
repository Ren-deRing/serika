#include <serika/cpu.h>
#include <serika/errno.h>
#include <serika/mm.h>
#include <serika/printk.h>
#include <serika/string.h>
#include <serika/syscall.h>
#include <serika/task.h>

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
