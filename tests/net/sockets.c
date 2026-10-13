#include <serika/net.h>
#include <serika/printk.h>
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

/* TCP through glibc over lo: the calls, ends, errors, and megabytes echoed through threads (also with a reader that waits) */
bool test_exec_tcp() {
    long status = 0;

    TEST_ASSERT(run_exec_mode_ms("/bin/tcp", "calls", 60000, &status) > 0);
    TEST_ASSERT(WEXITSTATUS(status) == 0x5e);
    return true;
}

/* ... while lo loses every 40th packet: retransmits (timer and fast), recovery, and the same bytes in the end */
bool test_exec_tcp_loss() {
    long status = 0;

    loop_set_drop(40);

    long reaped = run_exec_mode_ms("/bin/tcp", "loss", 90000, &status);

    loop_set_drop(0);

    TEST_ASSERT(reaped > 0);
    TEST_ASSERT(WEXITSTATUS(status) == 0x5e);
    printk("net: %u packets of lo dropped\n", loop_dropped());
    return true;
}

/* a web page from the internet through QEMU's user network (skipped without a NIC, and when the host has no network) */
bool test_exec_tcp_net() {
    long status = 0;

    TEST_ASSERT(run_exec_mode_ms("/bin/tcp", "net", 60000, &status) > 0);
    TEST_ASSERT(WEXITSTATUS(status) == 0x5e);
    return true;
}
