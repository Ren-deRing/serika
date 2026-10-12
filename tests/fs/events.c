#include <serika/syscall.h>

#include <tests/test.h>

/* epoll, eventfd, timerfd, signalfd and flock, through glibc: waiting, levels, one-shots, nesting, times, signals, locks */
bool test_exec_events() {
    long status = 0;

    TEST_ASSERT(run_exec("/bin/events", &status) > 0);
    TEST_ASSERT(WEXITSTATUS(status) == 0x5e);
    return true;
}
