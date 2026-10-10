#include <serika/errno.h>
#include <serika/cpu.h>
#include <serika/printk.h>
#include <serika/syscall.h>
#include <serika/task.h>

static long sys_exit(long code) {
    printk("task %lu exited with code %ld\n", curcpu->rq.curr->id, code);

    task_exit();
    return 0;
}

long syscall_dispatch(long nr, const long args[6]) {
    switch (nr) {
        case SYS_EXIT: return sys_exit(args[0]);
    }

    return -ENOSYS;
}
