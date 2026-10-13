#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* AF_INET and UDP, over lo, and with a NIC the DNS server of QEMU's user network. each failure is a number */

static struct sockaddr_in lo(int port) {
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };

    return a;
}

int main(void) {
    char buf[2048];
    int one = 1;

    int a = socket(AF_INET, SOCK_DGRAM, 0), b = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, IPPROTO_UDP);

    if (a < 0 || b < 0) return 1;
    if (socket(AF_INET, SOCK_STREAM, 0) != -1 && errno != ESOCKTNOSUPPORT) return 2;
    if (socket(AF_INET, SOCK_RAW, 0) != -1) return 3;

    /* a name: the port is one that was picked, and a second bind to it is an error */
    struct sockaddr_in sa = lo(0), sb = lo(0), from;
    socklen_t len = sizeof(sa);

    if (bind(a, (struct sockaddr *)&sa, sizeof(sa)) != 0) return 4;
    if (getsockname(a, (struct sockaddr *)&sa, &len) != 0 || len != sizeof(sa)) return 5;
    if (sa.sin_family != AF_INET || ntohs(sa.sin_port) < 49152 || sa.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) return 6;

    if (bind(b, (struct sockaddr *)&sa, sizeof(sa)) != -1 || errno != EADDRINUSE) return 7;
    if (bind(a, (struct sockaddr *)&sa, sizeof(sa)) != -1 || errno != EINVAL) return 8;

    struct sockaddr_in bad = lo(0);

    bad.sin_addr.s_addr = inet_addr("8.8.8.8");
    if (bind(b, (struct sockaddr *)&bad, sizeof(bad)) != -1 || errno != EADDRNOTAVAIL) return 9;

    /* nothing to read yet */
    if (recv(b, buf, sizeof(buf), 0) != -1 || errno != EAGAIN) return 10;

    /* b sends to a with no name of its own: it gets one */
    if (sendto(b, "hello", 5, 0, (struct sockaddr *)&sa, sizeof(sa)) != 5) return 11;

    struct pollfd pfd = { .fd = a, .events = POLLIN };

    if (poll(&pfd, 1, 2000) != 1 || !(pfd.revents & POLLIN)) return 12;

    int n = 0;

    if (ioctl(a, FIONREAD, &n) != 0 || n != 5) return 13;

    /* a peek leaves it, a short buffer cuts it and says so */
    len = sizeof(from);
    if (recvfrom(a, buf, 3, MSG_PEEK, (struct sockaddr *)&from, &len) != 3 || memcmp(buf, "hel", 3) != 0) return 14;

    struct iovec iov = { buf, 3 };
    struct msghdr mh = { .msg_name = &from, .msg_namelen = sizeof(from), .msg_iov = &iov, .msg_iovlen = 1 };

    if (recvmsg(a, &mh, 0) != 3 || !(mh.msg_flags & MSG_TRUNC) || mh.msg_namelen != sizeof(from)) return 15;
    if (from.sin_family != AF_INET || from.sin_addr.s_addr != htonl(INADDR_LOOPBACK) || ntohs(from.sin_port) < 49152) return 16;

    sb = from;

    /* the reply to whoever it was, and the other way with connect */
    if (sendto(a, "back", 4, 0, (struct sockaddr *)&sb, sizeof(sb)) != 4) return 17;

    usleep(50000);
    if (recv(b, buf, sizeof(buf), 0) != 4 || memcmp(buf, "back", 4) != 0) return 18;

    if (connect(b, (struct sockaddr *)&sa, sizeof(sa)) != 0) return 19;

    len = sizeof(from);
    if (getpeername(b, (struct sockaddr *)&from, &len) != 0 || from.sin_port != sa.sin_port) return 20;

    if (send(b, "conn", 4, 0) != 4) return 21;
    if (write(b, "wr", 2) != 2) return 22;

    usleep(50000);
    if (read(a, buf, sizeof(buf)) != 4 || memcmp(buf, "conn", 4) != 0) return 23;
    if (read(a, buf, sizeof(buf)) != 2 || memcmp(buf, "wr", 2) != 0) return 24;

    /* a datagram too long for one packet, and an empty one */
    static char big[2000];

    if (sendto(a, big, sizeof(big), 0, (struct sockaddr *)&sb, sizeof(sb)) != -1 || errno != EMSGSIZE) return 25;
    if (sendto(a, "", 0, 0, (struct sockaddr *)&sb, sizeof(sb)) != 0) return 26;

    usleep(50000);
    if (recv(b, buf, sizeof(buf), 0) != 0) return 27;

    /* a wait with a time limit */
    struct timeval tv = { 0, 100000 };

    if (setsockopt(a, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) return 28;
    if (recv(a, buf, sizeof(buf), 0) != -1 || errno != EAGAIN) return 29;

    /* options told back */
    int opt = 0;
    socklen_t ol = sizeof(opt);

    if (setsockopt(a, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0) return 30;
    if (getsockopt(a, SOL_SOCKET, SO_REUSEADDR, &opt, &ol) != 0 || opt != 1) return 31;
    if (getsockopt(a, SOL_SOCKET, SO_TYPE, &opt, &ol) != 0 || opt != SOCK_DGRAM) return 32;
    if (getsockopt(a, SOL_SOCKET, SO_DOMAIN, &opt, &ol) != 0 || opt != AF_INET) return 33;
    if (getsockopt(a, SOL_SOCKET, SO_PROTOCOL, &opt, &ol) != 0 || opt != IPPROTO_UDP) return 34;
    if (listen(a, 1) != -1 || errno != EOPNOTSUPP) return 35;

    /* a broadcast has to be asked for */
    struct sockaddr_in bc = lo(9);

    bc.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    if (sendto(a, "x", 1, 0, (struct sockaddr *)&bc, sizeof(bc)) != -1 || errno != EACCES) return 36;

    /* to a port nobody has: ICMP port unreachable goes back to nobody that listens for it (errors do not reach sockets) */
    close(b);

    int c = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in none = lo(1);

    if (sendto(c, "z", 1, 0, (struct sockaddr *)&none, sizeof(none)) != 1) return 37;

    /* the interfaces */
    struct ifreq ifr;

    memset(&ifr, 0, sizeof(ifr));
    strcpy(ifr.ifr_name, "lo");

    if (ioctl(c, SIOCGIFADDR, &ifr) != 0 || ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr.s_addr != htonl(INADDR_LOOPBACK)) return 38;
    if (ioctl(c, SIOCGIFNETMASK, &ifr) != 0 || ((struct sockaddr_in *)&ifr.ifr_netmask)->sin_addr.s_addr != htonl(0xff000000)) return 39;
    if (ioctl(c, SIOCGIFFLAGS, &ifr) != 0 || !(ifr.ifr_flags & IFF_LOOPBACK) || !(ifr.ifr_flags & IFF_UP)) return 40;
    if (ioctl(c, SIOCGIFINDEX, &ifr) != 0 || ifr.ifr_ifindex != 1) return 41;

    strcpy(ifr.ifr_name, "nope0");
    if (ioctl(c, SIOCGIFADDR, &ifr) != -1 || errno != ENODEV) return 42;

    struct ifreq list[8];
    struct ifconf ifc = { .ifc_len = sizeof(list), .ifc_req = list };

    if (ioctl(c, SIOCGIFCONF, &ifc) != 0 || ifc.ifc_len < (int)sizeof(struct ifreq) || strcmp(list[0].ifr_name, "lo") != 0) return 43;

    /* with a NIC: a question to the DNS server of QEMU's user network, whose answer is whatever it is, but an answer */
    strcpy(ifr.ifr_name, "eth0");

    if (ioctl(c, SIOCGIFADDR, &ifr) == 0) {
        if (((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr.s_addr == 0) return 44;

        unsigned char q[] = { 0x12, 0x34, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0,
                              7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'c', 'o', 'm', 0, 0, 1, 0, 1 };
        struct sockaddr_in dns = { .sin_family = AF_INET, .sin_port = htons(53) };

        dns.sin_addr.s_addr = inet_addr("10.0.2.3");
        tv.tv_sec = 3;
        tv.tv_usec = 0;

        if (setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) return 45;
        if (connect(c, (struct sockaddr *)&dns, sizeof(dns)) != 0) return 46;
        if (send(c, q, sizeof(q), 0) != (ssize_t)sizeof(q)) return 47;

        n = recv(c, buf, sizeof(buf), 0);
        if (n < 12) return 48;
        if ((unsigned char)buf[0] != 0x12 || (unsigned char)buf[1] != 0x34 || !((unsigned char)buf[2] & 0x80)) return 49;
    }

    close(a);
    close(c);
    return 0x5e;
}
