#include <serika/syscall.h>

#include <tests/test.h>

/* fork and copy-on-write both ways, wait4, execve and close-on-exec, through glibc */
bool test_fork_glibc() {
    long status = 0;

    TEST_ASSERT(run_exec("/bin/fork", &status) > 0);
    TEST_ASSERT(WEXITSTATUS(status) == 0x5e);
    return true;
}
