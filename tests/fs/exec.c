#include <serika/errno.h>
#include <serika/exec.h>
#include <serika/sched.h>
#include <serika/syscall.h>

#include <tests/test.h>

bool test_exec() {
    long status = 0;
    struct task *t;

    TEST_ASSERT(run_exec("/bin/check", &status) > 0);
    TEST_ASSERT(WEXITSTATUS(status) == 0x5e);

    /* static-pie, at ELF_ET_DYN_BASE */
    TEST_ASSERT(run_exec("/bin/check-pie", &status) > 0);
    TEST_ASSERT(WEXITSTATUS(status) == 0x5e);

    TEST_ASSERT(task_create_exec("/bin/nope", NULL, NULL, 0, &t) == -ENOENT);
    TEST_ASSERT(task_create_exec("/etc/motd", NULL, NULL, 0, &t) == -ENOEXEC);
    TEST_ASSERT(task_create_exec("/bin", NULL, NULL, 0, &t) == -EACCES);
    return true;
}

bool test_exec_glibc() {
    long status = 0;

    TEST_ASSERT(run_exec("/bin/hello", &status) > 0);
    TEST_ASSERT(WEXITSTATUS(status) == 0x5e);
    return true;
}

/* files, the console, time and munmap, through glibc */
bool test_exec_files() {
    long status = 0;

    TEST_ASSERT(run_exec("/bin/files", &status) > 0);
    TEST_ASSERT(WEXITSTATUS(status) == 0x5e);
    return true;
}
