#include <serika/errno.h>
#include <serika/syscall.h>
#include <serika/task.h>

static long sys_exit(long code) {
    task_exit((code & 0xff) << 8); /* wait status: exit code in bits 15..8 */
    return 0;
}

long syscall_dispatch(long nr, const long args[6]) {
    switch (nr) {
        case SYS_EXIT: return sys_exit(args[0]);
    }

    return -ENOSYS;
}
