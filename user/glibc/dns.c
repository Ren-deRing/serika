#define _GNU_SOURCE
#include <arpa/inet.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* names, through glibc's resolver: /etc/hosts, a number, and with a NIC a name from the DNS server of QEMU's user network */

static unsigned addr_of(const struct addrinfo *ai) {
    return ((const struct sockaddr_in *)ai->ai_addr)->sin_addr.s_addr;
}

int main(void) {
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM }, *res = NULL;

    /* /etc/hosts */
    int r = getaddrinfo("localhost", "53", &hints, &res);

    if (r != 0 || !res) return 1;
    if (res->ai_family != AF_INET || addr_of(res) != htonl(INADDR_LOOPBACK)) return 2;
    if (((struct sockaddr_in *)res->ai_addr)->sin_port != htons(53)) return 3;

    freeaddrinfo(res);

    /* a number needs no server */
    hints.ai_flags = AI_NUMERICHOST;

    if (getaddrinfo("10.1.2.3", NULL, &hints, &res) != 0 || addr_of(res) != htonl(0x0a010203)) return 4;

    freeaddrinfo(res);

    if (getaddrinfo("not-a-number", NULL, &hints, &res) != EAI_NONAME) return 5;

    hints.ai_flags = 0;

    struct hostent *he = gethostbyname("localhost");

    if (!he || he->h_addrtype != AF_INET || *(unsigned *)he->h_addr_list[0] != htonl(INADDR_LOOPBACK)) return 6;

    char name[NI_MAXHOST];
    struct sockaddr_in sin = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };

    if (getnameinfo((struct sockaddr *)&sin, sizeof(sin), name, sizeof(name), NULL, 0, NI_NAMEREQD) != 0 ||
        strcmp(name, "localhost") != 0) return 7;

    /* with a NIC: example.com. the server's answer is whatever the host's network lets it be (it may be that there is none),
     * but an answer, and not the end of a wait of seconds, shows the stack works. the address is printed */
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq ifr;

    memset(&ifr, 0, sizeof(ifr));
    strcpy(ifr.ifr_name, "eth0");

    int nic = ioctl(s, SIOCGIFADDR, &ifr) == 0;

    close(s);

    if (nic) {
        struct timespec t0, t1;

        clock_gettime(CLOCK_MONOTONIC, &t0);
        r = getaddrinfo("example.com", NULL, &hints, &res);
        clock_gettime(CLOCK_MONOTONIC, &t1);

        long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;

        if (r == 0) {
            char text[INET_ADDRSTRLEN];

            inet_ntop(AF_INET, &((struct sockaddr_in *)res->ai_addr)->sin_addr, text, sizeof(text));
            printf("dns: example.com is %s (%ld ms)\n", text, ms);
            freeaddrinfo(res);
        } else {
            printf("dns: example.com: %s (%ld ms)\n", gai_strerror(r), ms);

            /* the server had to answer, and soon: a timeout is the stack's */
            if (r != EAI_NONAME && r != EAI_NODATA && r != EAI_AGAIN) return 8;
            if (ms > 4000) return 9;
        }
    }

    return 0x5e;
}
