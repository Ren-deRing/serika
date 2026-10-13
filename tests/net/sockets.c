#include <serika/syscall.h>

#include <tests/test.h>

/* unix sockets, through glibc: stream, datagram and seqpacket, names, passing files, options, waiting */
bool test_exec_sockets() {
    long status = 0;

    TEST_ASSERT(run_exec("/bin/sockets", &status) > 0);
    TEST_ASSERT(WEXITSTATUS(status) == 0x5e);
    return true;
}

/* AF_INET and UDP, through glibc: names, datagrams, peek and truncation, connect, options, the interfaces, and with a NIC the DNS server */
bool test_exec_udp() {
    long status = 0;

    TEST_ASSERT(run_exec("/bin/udp", &status) > 0);
    TEST_ASSERT(WEXITSTATUS(status) == 0x5e);
    return true;
}

/* names through glibc's resolver: /etc/hosts, numbers, and with a NIC example.com from QEMU's DNS server */
bool test_exec_dns() {
    long status = 0;

    TEST_ASSERT(run_exec("/bin/dns", &status) > 0);
    TEST_ASSERT(WEXITSTATUS(status) == 0x5e);
    return true;
}
