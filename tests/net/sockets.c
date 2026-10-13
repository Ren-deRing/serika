#include <serika/fcntl.h>
#include <serika/net.h>
#include <serika/printk.h>
#include <serika/syscall.h>
#include <serika/vfs.h>

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

/* the host's openssl runs (busybox's wget starts it for https): its libraries load and it says its version */
bool test_exec_openssl() {
    struct file *f;
    long status = 0;

    if (vfs_open("/usr/bin/openssl", O_RDONLY, 0, &f) != 0) {
        printk("openssl: not on /usr -- skipped\n");
        return true;
    }

    vfs_close(f);

    TEST_ASSERT(run_exec_mode_ms("/usr/bin/openssl", "version", 20000, &status) > 0);
    TEST_ASSERT(WEXITSTATUS(status) == 0);
    return true;
}

/* python, from the disk: its library loads, and a script (/usr/hello.py) does arithmetic, files, threads, a tcp echo on lo and a
 * child process. skipped without it */
bool test_exec_python() {
    struct file *f;
    long status = 0;

    if (vfs_open("/usr/bin/python3", O_RDONLY, 0, &f) != 0) {
        printk("python: not on /usr -- skipped\n");
        return true;
    }

    vfs_close(f);

    TEST_ASSERT(run_exec_mode_ms("/usr/bin/python3", "/usr/hello.py", 120000, &status) > 0);
    TEST_ASSERT(WEXITSTATUS(status) == 0x5e);
    return true;
}
