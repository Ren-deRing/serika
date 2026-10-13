#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* TCP through glibc. argv[1]: "net" gets a web page from the internet, "loss" moves a lot of data while the kernel drops
 * packets on lo, anything else is the tests of the calls and of a transfer over lo. each failure is a number */

#define CHECK(cond, code) do { if (!(cond)) { fprintf(stderr, "tcp: check %d failed (errno %d)\n", code, errno); return code; } } while (0)

static volatile int failed;

static struct sockaddr_in lo(int port) {
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };

    return a;
}

/* a listener on lo with a port of its own */
static int listener(int backlog, int rcvbuf, int *port) {
    int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    struct sockaddr_in a = lo(0);
    socklen_t len = sizeof(a);

    if (s < 0) return -1;

    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    if (rcvbuf) setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(s, backlog) != 0) return -1;
    if (getsockname(s, (struct sockaddr *)&a, &len) != 0) return -1;

    *port = ntohs(a.sin_port);
    return s;
}

static int dial(int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = lo(port);

    if (s < 0 || connect(s, (struct sockaddr *)&a, sizeof(a)) != 0) return -1;

    return s;
}

static unsigned char pattern(size_t i) {
    return (i * 7 + 3) & 0xff;
}

static int write_all(int fd, const void *buf, size_t n) {
    for (size_t off = 0; off < n;) {
        ssize_t w = write(fd, (const char *)buf + off, n - off);

        if (w <= 0) return -1;

        off += w;
    }

    return 0;
}

/* ---- threads ---- */

static void *echo(void *arg) {
    int fd = (int)(long)arg;
    char buf[16384];

    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));

        if (n < 0) {
            failed = 1;
            break;
        }

        if (n == 0) break;

        if (write_all(fd, buf, n) != 0) {
            failed = 2;
            break;
        }
    }

    shutdown(fd, SHUT_WR);
    close(fd);
    return NULL;
}

struct source {
    int    fd;
    size_t total;
};

static void *source(void *arg) {
    struct source *s = arg;
    static unsigned char chunk[8192];
    size_t sent = 0;

    while (sent < s->total) {
        size_t n = s->total - sent < sizeof(chunk) ? s->total - sent : sizeof(chunk);

        for (size_t i = 0; i < n; i++) chunk[i] = pattern(sent + i);

        if (write_all(s->fd, chunk, n) != 0) {
            failed = 3;
            return NULL;
        }

        sent += n;
    }

    shutdown(s->fd, SHUT_WR);
    return NULL;
}

/* total bytes of the pattern to a server that echoes them, and back, intact. the time it took, in ms, or -code */
static long transfer(size_t total, int rcvbuf) {
    int port;
    int ls = listener(4, rcvbuf, &port);

    if (ls < 0) return -1;

    int c = dial(port);

    if (c < 0) return -2;

    int a = accept(ls, NULL, NULL);

    if (a < 0) return -3;

    struct timespec t0, t1;

    clock_gettime(CLOCK_MONOTONIC, &t0);

    pthread_t te, ts;
    struct source src = { c, total };

    pthread_create(&te, NULL, echo, (void *)(long)a);
    pthread_create(&ts, NULL, source, &src);

    static unsigned char buf[16384];
    size_t got = 0;

    for (;;) {
        ssize_t n = read(c, buf, sizeof(buf));

        if (n < 0) return -4;
        if (n == 0) break;

        for (ssize_t i = 0; i < n; i++)
            if (buf[i] != pattern(got + i)) return -5;

        if ((got + n) / (512 * 1024) != got / (512 * 1024)) fprintf(stderr, "tcp: %zu KiB back\n", (got + n) / 1024);

        got += n;
    }

    pthread_join(ts, NULL);
    pthread_join(te, NULL);

    clock_gettime(CLOCK_MONOTONIC, &t1);

    close(c);
    close(ls);

    if (failed) return -6;
    if (got != total) return -7;

    return (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
}

/* ---- modes ---- */

static int calls(void) {
    int port, one = 1, opt = 0;
    socklen_t ol = sizeof(opt);
    char buf[64];

    int ls = listener(4, 0, &port);

    CHECK(ls >= 0, 1);
    CHECK(getsockopt(ls, SOL_SOCKET, SO_ACCEPTCONN, &opt, &ol) == 0 && opt == 1, 2);
    CHECK(getsockopt(ls, SOL_SOCKET, SO_TYPE, &opt, &ol) == 0 && opt == SOCK_STREAM, 3);
    CHECK(getsockopt(ls, SOL_SOCKET, SO_PROTOCOL, &opt, &ol) == 0 && opt == IPPROTO_TCP, 4);

    /* the port is taken, and a datagram socket cannot listen, nor a stream socket be sent to by name */
    int dup = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = lo(port);

    CHECK(bind(dup, (struct sockaddr *)&a, sizeof(a)) == -1 && errno == EADDRINUSE, 5);
    CHECK(listen(dup, 1) == 0, 6);                     /* an unbound socket gets a port */
    close(dup);

    /* nothing to accept */
    int fl = fcntl(ls, F_GETFL);

    fcntl(ls, F_SETFL, fl | O_NONBLOCK);
    CHECK(accept(ls, NULL, NULL) == -1 && errno == EAGAIN, 7);
    fcntl(ls, F_SETFL, fl);

    struct pollfd pfd = { .fd = ls, .events = POLLIN };

    CHECK(poll(&pfd, 1, 0) == 0, 8);

    /* a port with nobody: refused, blocking and not */
    struct sockaddr_in none = lo(1);
    int c = socket(AF_INET, SOCK_STREAM, 0);

    CHECK(connect(c, (struct sockaddr *)&none, sizeof(none)) == -1 && errno == ECONNREFUSED, 9);
    CHECK(connect(c, (struct sockaddr *)&none, sizeof(none)) == -1 && errno == ECONNREFUSED, 10);   /* and again, on the same socket */
    close(c);

    c = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);

    int r = connect(c, (struct sockaddr *)&none, sizeof(none));

    CHECK(r == -1 && (errno == EINPROGRESS || errno == ECONNREFUSED), 11);

    pfd = (struct pollfd){ .fd = c, .events = POLLOUT };
    CHECK(poll(&pfd, 1, 2000) == 1 && (pfd.revents & (POLLERR | POLLHUP)), 12);
    CHECK(getsockopt(c, SOL_SOCKET, SO_ERROR, &opt, &ol) == 0 && opt == ECONNREFUSED, 13);
    CHECK(getsockopt(c, SOL_SOCKET, SO_ERROR, &opt, &ol) == 0 && opt == 0, 14);     /* told once */
    close(c);

    /* a connection: both ends know the same names */
    c = dial(port);
    CHECK(c >= 0, 15);

    struct sockaddr_in ca, sa, pa;
    socklen_t l1 = sizeof(ca), l2 = sizeof(sa), l3 = sizeof(pa);
    int s = accept(ls, (struct sockaddr *)&pa, &l3);

    CHECK(s >= 0 && l3 == sizeof(pa), 16);
    CHECK(getsockname(c, (struct sockaddr *)&ca, &l1) == 0 && getpeername(s, (struct sockaddr *)&sa, &l2) == 0, 17);
    CHECK(ca.sin_port == sa.sin_port && ca.sin_port == pa.sin_port && ca.sin_addr.s_addr == htonl(INADDR_LOOPBACK), 18);
    CHECK(connect(c, (struct sockaddr *)&a, sizeof(a)) == -1 && errno == EISCONN, 19);

    /* words there and back, peeked and read, and what is waiting is told */
    CHECK(setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) == 0, 20);
    CHECK(getsockopt(c, IPPROTO_TCP, TCP_NODELAY, &opt, &ol) == 0 && opt == 1, 21);
    CHECK(send(c, "hello world", 11, 0) == 11, 22);

    pfd = (struct pollfd){ .fd = s, .events = POLLIN };
    CHECK(poll(&pfd, 1, 2000) == 1 && (pfd.revents & POLLIN), 23);

    int n = 0;

    usleep(50000);
    CHECK(ioctl(s, FIONREAD, &n) == 0 && n == 11, 24);
    CHECK(recv(s, buf, 5, MSG_PEEK) == 5 && memcmp(buf, "hello", 5) == 0, 25);
    CHECK(recv(s, buf, 3, 0) == 3 && memcmp(buf, "hel", 3) == 0, 26);
    CHECK(recv(s, buf, sizeof(buf), 0) == 8 && memcmp(buf, "lo world", 8) == 0, 27);

    fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
    CHECK(recv(s, buf, sizeof(buf), 0) == -1 && errno == EAGAIN, 28);
    fcntl(s, F_SETFL, fcntl(s, F_GETFL) & ~O_NONBLOCK);

    /* all of it asked for: the first part waits for the second */
    CHECK(send(c, "12345", 5, 0) == 5, 29);
    usleep(100000);
    CHECK(send(c, "6789", 4, 0) == 4, 30);
    CHECK(recv(s, buf, 9, MSG_WAITALL) == 9 && memcmp(buf, "123456789", 9) == 0, 31);

    /* a wait with a limit */
    struct timeval tv = { 0, 100000 };

    CHECK(setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0, 32);
    CHECK(recv(s, buf, sizeof(buf), 0) == -1 && errno == EAGAIN, 33);

    /* one end shuts its writing: the other reads the end, can still send, and is told with POLLRDHUP */
    CHECK(shutdown(c, SHUT_WR) == 0, 34);
    CHECK(recv(s, buf, sizeof(buf), 0) == 0, 35);

    pfd = (struct pollfd){ .fd = s, .events = POLLIN | POLLRDHUP };
    CHECK(poll(&pfd, 1, 1000) == 1 && (pfd.revents & POLLIN) && (pfd.revents & POLLRDHUP), 36);
    CHECK(send(s, "back", 4, 0) == 4, 37);
    CHECK(recv(c, buf, sizeof(buf), 0) == 4 && memcmp(buf, "back", 4) == 0, 38);
    CHECK(send(c, "x", 1, MSG_NOSIGNAL) == -1 && errno == EPIPE, 39);

    /* and the end of the rest */
    close(s);
    CHECK(recv(c, buf, sizeof(buf), 0) == 0, 40);
    close(c);

    /* the peer is gone: the first send is let go, the answer to it (a reset) makes the next fail */
    c = dial(port);
    s = accept(ls, NULL, NULL);
    CHECK(c >= 0 && s >= 0, 41);
    close(s);
    usleep(100000);

    send(c, "x", 1, MSG_NOSIGNAL);
    usleep(100000);
    CHECK(send(c, "y", 1, MSG_NOSIGNAL) == -1 && (errno == EPIPE || errno == ECONNRESET), 42);
    close(c);

    /* close with unread data is a reset for the other end */
    c = dial(port);
    s = accept(ls, NULL, NULL);
    CHECK(send(c, "unread", 6, 0) == 6, 43);
    usleep(100000);
    close(s);
    usleep(100000);
    CHECK(recv(c, buf, sizeof(buf), 0) == -1 && errno == ECONNRESET, 44);
    close(c);

    /* several at once wait in the queue */
    int cs[3];

    for (int i = 0; i < 3; i++) cs[i] = dial(port);
    for (int i = 0; i < 3; i++) {
        CHECK(cs[i] >= 0, 45);
        CHECK(send(cs[i], &i, sizeof(i), 0) == sizeof(i), 46);
    }

    for (int i = 0; i < 3; i++) {
        int v = -1, a2 = accept(ls, NULL, NULL);

        CHECK(a2 >= 0 && recv(a2, &v, sizeof(v), MSG_WAITALL) == sizeof(v) && v == i, 47);
        close(a2);
        close(cs[i]);
    }

    close(ls);
    return 0;
}

static int transfers(int loss) {
    long ms = transfer(loss ? 256 * 1024 : 4 * 1024 * 1024, 0);

    CHECK(ms >= 0, 60 - (int)ms);
    printf("tcp: %s %s transfer in %ld ms\n", loss ? "256 KiB" : "4 MiB", loss ? "with losses," : "echoed,", ms);

    if (loss) return 0;

    /* a receiver with a small buffer: the window is small, and it is shut while the reader waits */
    int port;
    int ls = listener(4, 8192, &port);

    CHECK(ls >= 0, 70);

    int c = dial(port), a = accept(ls, NULL, NULL);

    CHECK(c >= 0 && a >= 0, 71);

    pthread_t ts;
    struct source src = { c, 400 * 1024 };

    pthread_create(&ts, NULL, source, &src);
    usleep(500000);                                    /* nobody reads: the buffers fill, the window closes */

    static unsigned char buf[16384];
    size_t got = 0;

    for (;;) {
        ssize_t n = read(a, buf, sizeof(buf));

        CHECK(n >= 0, 72);

        if (n == 0) break;

        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] != pattern(got + i)) {
                fprintf(stderr, "tcp: byte %zu is wrong\n", got + i);
                return 73;
            }
        }

        got += n;
    }

    pthread_join(ts, NULL);
    CHECK(got == 400 * 1024 && !failed, 74);
    close(a);
    close(c);
    close(ls);
    return 0;
}

static int web(void) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq ifr;

    memset(&ifr, 0, sizeof(ifr));
    strcpy(ifr.ifr_name, "eth0");

    int nic = ioctl(s, SIOCGIFADDR, &ifr) == 0;

    close(s);

    if (!nic) {
        printf("tcp: no NIC -- skipped\n");
        return 0;
    }

    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res;
    int r = getaddrinfo("example.com", "80", &hints, &res);

    if (r != 0) {
        printf("tcp: example.com: %s -- no internet?\n", gai_strerror(r));
        return 0;
    }

    int c = socket(AF_INET, SOCK_STREAM, 0);
    struct timeval tv = { 10, 0 };

    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(c, res->ai_addr, res->ai_addrlen) != 0) {
        printf("tcp: connect to example.com: %s -- no internet?\n", strerror(errno));
        return 0;
    }

    const char req[] = "GET / HTTP/1.0\r\nHost: example.com\r\nConnection: close\r\n\r\n";

    CHECK(write_all(c, req, sizeof(req) - 1) == 0, 80);

    static char page[65536];
    size_t got = 0;

    for (;;) {
        ssize_t n = read(c, page + got, sizeof(page) - 1 - got);

        if (n <= 0) break;

        got += n;
    }

    page[got] = '\0';

    CHECK(got > 100 && strncmp(page, "HTTP/1.", 7) == 0, 81);

    char *eol = strchr(page, '\r');

    if (eol) *eol = '\0';

    printf("tcp: example.com says \"%s\" (%zu bytes)\n", page, got);
    close(c);
    freeaddrinfo(res);
    return 0;
}

int main(int argc, char **argv) {
    int r;

    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc > 1 && strcmp(argv[1], "net") == 0) {
        r = web();
    } else if (argc > 1 && strcmp(argv[1], "loss") == 0) {
        r = transfers(1);
    } else {
        r = calls();

        if (!r) r = transfers(0);
    }

    return r ? r : 0x5e;
}
