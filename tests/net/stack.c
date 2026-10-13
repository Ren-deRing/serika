#include <serika/errno.h>
#include <serika/net.h>
#include <serika/printk.h>
#include <serika/sched.h>
#include <serika/string.h>
#include <serika/timer.h>

#include <asm/arch.h>
#include <serika/cpu.h>
#include <tests/test.h>

/* the ipv4 base: a ping to ourselves through lo, and (with a NIC, QEMU's user network) ARP and a ping to the gateway */

static bool link_up(struct netdev *dev) {
    for (int i = 0; i < 30; i++) {
        if (dev->flags & NETDEV_LINK) return true;

        sleep_until(rdtsc() + this_cpu_read(tsc_freq_hz) / 10);
    }

    return false;
}

bool test_net_ping_lo() {
    uint32_t rtt = 0;

    struct netdev *lo = netdev_find("lo");
    TEST_ASSERT(lo && lo->ip == IP4(127, 0, 0, 1) && (lo->flags & NETDEV_LOOPBACK));

    TEST_ASSERT(icmp_ping(IP4(127, 0, 0, 1), 1000, &rtt) == 0);
    printk("net: ping 127.0.0.1 %u us\n", rtt);

    /* another address of the same host goes the same way */
    TEST_ASSERT(icmp_ping(IP4(127, 1, 2, 3), 1000, &rtt) == 0);

    /* the checksum of a header, once as the stack has it */
    uint8_t hdr[20] = { 0x45, 0, 0, 0x73, 0, 0, 0x40, 0, 0x40, 0x11, 0, 0, 0xc0, 0xa8, 0, 1, 0xc0, 0xa8, 0, 0xc7 };
    uint16_t csum = net_csum(hdr, sizeof(hdr), 0);

    TEST_ASSERT(csum == htons(0xb861));
    memcpy(hdr + 10, &csum, 2);
    TEST_ASSERT(net_csum(hdr, sizeof(hdr), 0) == 0);
    return true;
}

bool test_net_arp() {
    struct netdev *dev = netdev_find("eth0");

    if (!dev) {
        printk("net: no NIC -- skipped\n");
        return true;
    }

    TEST_ASSERT(link_up(dev));
    TEST_ASSERT(dev->ip == IP4(10, 0, 2, 15) && dev->gw == IP4(10, 0, 2, 2));

    /* a ping makes the stack ask for the gateway's address */
    TEST_ASSERT(icmp_ping(dev->gw, 3000, NULL) == 0);

    uint8_t mac[ETH_ALEN];

    mutex_lock(&net_lock);
    bool known = arp_lookup(dev->gw, mac);
    mutex_unlock(&net_lock);

    TEST_ASSERT(known);
    printk("net: gateway is at %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    TEST_ASSERT(mac[0] == 0x52 && mac[1] == 0x55);        /* QEMU's user network */

    /* an address nobody has: three requests, then the packet is dropped and the ping runs out */
    TEST_ASSERT(icmp_ping(IP4(10, 0, 2, 99), 1500, NULL) == -ETIMEDOUT);

    mutex_lock(&net_lock);
    known = arp_lookup(IP4(10, 0, 2, 99), mac);
    mutex_unlock(&net_lock);

    TEST_ASSERT(!known);
    return true;
}

bool test_net_ping_gw() {
    struct netdev *dev = netdev_find("eth0");

    if (!dev) {
        printk("net: no NIC -- skipped\n");
        return true;
    }

    uint32_t rtt = 0;

    TEST_ASSERT(link_up(dev));
    TEST_ASSERT(icmp_ping(dev->gw, 3000, &rtt) == 0);
    printk("net: ping 10.0.2.2 %u us\n", rtt);

    /* our own address is answered on lo */
    TEST_ASSERT(icmp_ping(dev->ip, 1000, &rtt) == 0);
    return true;
}

/* sockets of the kernel's own on lo: a datagram there and back, the port taken, nobody at a port, a wait that runs out */
bool test_net_udp() {
    struct isock *a = udp_open(), *b = udp_open(), *c = udp_open();
    char buf[64];
    uint32_t saddr = 0;
    uint16_t sport = 0;

    TEST_ASSERT(a && b && c);

    TEST_ASSERT(udp_bind_k(a, IP4(127, 0, 0, 1), 7000) == 0);
    TEST_ASSERT(udp_bind_k(b, 0, 0) == 0 && b->lport >= UDP_PORT_FIRST);
    TEST_ASSERT(udp_bind_k(c, 0, 7000) == -EADDRINUSE);
    TEST_ASSERT(udp_bind_k(c, IP4(8, 8, 8, 8), 0) == -EADDRNOTAVAIL);

    TEST_ASSERT(udp_send_k(b, IP4(127, 0, 0, 1), 7000, "ping", 4) == 0);
    TEST_ASSERT(udp_recv_k(a, buf, sizeof(buf), &saddr, &sport, 1000) == 4 && memcmp(buf, "ping", 4) == 0);
    TEST_ASSERT(saddr == IP4(127, 0, 0, 1) && sport == b->lport);

    /* back to where it came from */
    TEST_ASSERT(udp_send_k(a, saddr, sport, "pong", 4) == 0);
    TEST_ASSERT(udp_recv_k(b, buf, sizeof(buf), NULL, NULL, 1000) == 4 && memcmp(buf, "pong", 4) == 0);

    /* nobody at the port: the sender is told by ICMP and nothing is queued; nothing to read is a wait that runs out */
    TEST_ASSERT(udp_send_k(b, IP4(127, 0, 0, 1), 7001, "x", 1) == 0);
    TEST_ASSERT(udp_recv_k(b, buf, sizeof(buf), NULL, NULL, 200) == -ETIMEDOUT);

    TEST_ASSERT(udp_send_k(b, IP4(127, 0, 0, 1), 7000, buf, UDP_MAX + 1) == -EMSGSIZE);
    TEST_ASSERT(udp_send_k(b, IP4(127, 0, 0, 1), 0, "x", 1) == -EINVAL);

    udp_close(a);
    udp_close(b);
    udp_close(c);

    /* the port is free again */
    struct isock *d = udp_open();

    TEST_ASSERT(d && udp_bind_k(d, 0, 7000) == 0);
    udp_close(d);
    return true;
}
