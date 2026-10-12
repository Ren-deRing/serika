#include <serika/syscall.h>

#include <tests/test.h>

/* unix sockets, through glibc: stream, datagram and seqpacket, names, passing files, options, waiting */
bool test_exec_sockets() {
    long status = 0;

    TEST_ASSERT(run_exec("/bin/sockets", &status) > 0);
    TEST_ASSERT(WEXITSTATUS(status) == 0x5e);
    return true;
}
