#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PATH "/tmp/sockets.sock"
#define DGRAM_A "/tmp/sockets-a.sock"
#define DGRAM_B "/tmp/sockets-b.sock"

static struct sockaddr_un path_addr(const char *path, socklen_t *len) {
    struct sockaddr_un a = { .sun_family = AF_UNIX };

    strcpy(a.sun_path, path);
    *len = offsetof(struct sockaddr_un, sun_path) + strlen(path) + 1;
    return a;
}

/* "\0name": the name is exactly these bytes */
static struct sockaddr_un abstract_addr(const char *name, socklen_t *len) {
    struct sockaddr_un a = { .sun_family = AF_UNIX };

    memcpy(a.sun_path + 1, name, strlen(name));
    *len = offsetof(struct sockaddr_un, sun_path) + 1 + strlen(name);
    return a;
}

static int send_fds(int sock, const int *fds, int n, const char *text) {
    char control[CMSG_SPACE(sizeof(int) * 8)];
    struct iovec iov = { (void *)text, strlen(text) };
    struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = control,
                         .msg_controllen = CMSG_SPACE(sizeof(int) * n) };

    memset(control, 0, sizeof(control));

    struct cmsghdr *c = CMSG_FIRSTHDR(&mh);

    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int) * n);
    memcpy(CMSG_DATA(c), fds, sizeof(int) * n);

    return sendmsg(sock, &mh, 0);
}

/* the files that came with a message: how many, in out[], and the flags of the receive */
static int recv_fds(int sock, int *out, int max, char *text, size_t tlen, int flags, int *msg_flags, size_t ctl) {
    char control[CMSG_SPACE(sizeof(int) * 8)];
    struct iovec iov = { text, tlen };
    struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = ctl };

    int n = recvmsg(sock, &mh, flags);
    int got = 0;

    if (msg_flags) *msg_flags = mh.msg_flags;
    if (n < 0) return -1;

    for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            int k = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);

            for (int i = 0; i < k && got < max; i++) memcpy(&out[got++], CMSG_DATA(c) + i * sizeof(int), sizeof(int));
        }
    }

    text[n] = '\0';
    return got;
}

static long ms_since(const struct timespec *t0) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);

    return (t.tv_sec - t0->tv_sec) * 1000 + (t.tv_nsec - t0->tv_nsec) / 1000000;
}

/* the exit code is the number of the check that failed */
int main(void) {
    char buf[256];
    int sv[2], status;
    socklen_t len;

    signal(SIGPIPE, SIG_IGN);

    /* 1. a stream pair: data both ways, the end after shutdown */
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return 1;
    if (write(sv[0], "hello", 5) != 5 || read(sv[1], buf, sizeof(buf)) != 5 || memcmp(buf, "hello", 5)) return 2;
    if (write(sv[1], "back", 4) != 4 || read(sv[0], buf, sizeof(buf)) != 4) return 3;

    if (shutdown(sv[0], SHUT_WR) != 0 || read(sv[1], buf, sizeof(buf)) != 0) return 4; /* the end */
    if (write(sv[1], "more", 4) != 4 || read(sv[0], buf, sizeof(buf)) != 4) return 5;  /* the other way still goes */
    if (send(sv[0], "x", 1, MSG_NOSIGNAL) != -1 || errno != EPIPE) return 6;

    close(sv[0]);
    if (read(sv[1], buf, sizeof(buf)) != 0) return 7;
    if (send(sv[1], "x", 1, MSG_NOSIGNAL) != -1 || errno != EPIPE) return 8;
    close(sv[1]);

    /* 2. a listener on a path, a client in another process, who is who */
    unlink(PATH);

    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sa = path_addr(PATH, &len);

    if (srv < 0 || bind(srv, (struct sockaddr *)&sa, len) != 0) return 9;
    if (listen(srv, 4) != 0) return 10;

    int other = socket(AF_UNIX, SOCK_STREAM, 0);
    if (bind(other, (struct sockaddr *)&sa, len) != -1 || errno != EADDRINUSE) return 11;
    close(other);

    struct stat st;
    if (stat(PATH, &st) != 0 || !S_ISSOCK(st.st_mode)) return 12;
    if (open(PATH, O_RDWR) != -1 || errno != ENXIO) return 13;

    pid_t pid = fork();
    if (pid < 0) return 14;

    if (pid == 0) {
        int c = socket(AF_UNIX, SOCK_STREAM, 0);

        if (connect(c, (struct sockaddr *)&sa, len) != 0) _exit(1);
        if (write(c, "ping", 4) != 4 || read(c, buf, sizeof(buf)) != 4 || memcmp(buf, "pong", 4)) _exit(2);

        struct ucred cred;
        socklen_t cl = sizeof(cred);

        /* the other end of this is the parent */
        if (getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cred, &cl) != 0 || cred.pid != getppid()) _exit(3);

        struct sockaddr_un peer;
        socklen_t pl = sizeof(peer);

        if (getpeername(c, (struct sockaddr *)&peer, &pl) != 0 || strcmp(peer.sun_path, PATH)) _exit(4);

        _exit(0);
    }

    struct sockaddr_un from;
    socklen_t fl = sizeof(from);
    int conn = accept4(srv, (struct sockaddr *)&from, &fl, SOCK_CLOEXEC | SOCK_NONBLOCK);

    if (conn < 0 || fl != sizeof(sa.sun_family)) return 15; /* the client has no name */
    if (!(fcntl(conn, F_GETFD) & FD_CLOEXEC) || !(fcntl(conn, F_GETFL) & O_NONBLOCK)) return 16;

    /* it is nonblocking: nothing yet is EAGAIN, and then poll says when */
    struct pollfd pf = { conn, POLLIN, 0 };

    if (read(conn, buf, sizeof(buf)) != 4) {
        if (errno != EAGAIN) return 17;
        if (poll(&pf, 1, 2000) != 1 || !(pf.revents & POLLIN)) return 18;
        if (read(conn, buf, sizeof(buf)) != 4) return 19;
    }

    if (memcmp(buf, "ping", 4)) return 20;

    struct ucred cred;
    socklen_t cl = sizeof(cred);

    if (getsockopt(conn, SOL_SOCKET, SO_PEERCRED, &cred, &cl) != 0 || cred.pid != pid) return 21;

    struct sockaddr_un me;
    socklen_t ml = sizeof(me);

    if (getsockname(conn, (struct sockaddr *)&me, &ml) != 0 || strcmp(me.sun_path, PATH)) return 22;

    if (write(conn, "pong", 4) != 4) return 23;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return 24;

    /* the client is gone: the end of the stream, and writing is EPIPE */
    pf.revents = 0;
    if (poll(&pf, 1, 1000) != 1 || !(pf.revents & POLLIN)) return 25;
    if (read(conn, buf, sizeof(buf)) != 0) return 26;
    close(conn);

    /* connecting to what is not there, and to what is not a socket */
    struct sockaddr_un none = path_addr("/tmp/sockets-none", &len);
    int c = socket(AF_UNIX, SOCK_STREAM, 0);

    if (connect(c, (struct sockaddr *)&none, len) != -1 || errno != ENOENT) return 27;

    none = path_addr("/etc/motd", &len);
    if (connect(c, (struct sockaddr *)&none, len) != -1 || errno != ECONNREFUSED) return 28;
    close(c);

    /* a closed listener refuses; the path is still there until it is unlinked */
    close(srv);

    c = socket(AF_UNIX, SOCK_STREAM, 0);
    sa = path_addr(PATH, &len);
    if (connect(c, (struct sockaddr *)&sa, len) != -1 || errno != ECONNREFUSED) return 29;
    close(c);

    if (unlink(PATH) != 0) return 30;

    /* 3. abstract names, and a backlog that fills */
    struct sockaddr_un ab = abstract_addr("serika-test", &len);

    srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (bind(srv, (struct sockaddr *)&ab, len) != 0 || listen(srv, 1) != 0) return 31;

    other = socket(AF_UNIX, SOCK_STREAM, 0);
    if (bind(other, (struct sockaddr *)&ab, len) != -1 || errno != EADDRINUSE) return 32;
    close(other);

    me.sun_family = 0;
    ml = sizeof(me);
    if (getsockname(srv, (struct sockaddr *)&me, &ml) != 0 || ml != len || memcmp(me.sun_path, ab.sun_path, ml - 2)) return 33;

    int clients[4], ok = 0, again = 0;

    for (int i = 0; i < 4; i++) {
        clients[i] = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);

        if (connect(clients[i], (struct sockaddr *)&ab, len) == 0) ok++;
        else if (errno == EAGAIN) again++;
    }

    if (ok != 2 || again != 2) return 34; /* a backlog of 1 holds two */

    int a1 = accept(srv, NULL, NULL);
    if (a1 < 0 || connect(clients[3], (struct sockaddr *)&ab, len) != 0) return 35; /* room again */

    close(a1);
    for (int i = 0; i < 4; i++) close(clients[i]);

    int opt = 0;
    socklen_t ol = sizeof(opt);

    if (getsockopt(srv, SOL_SOCKET, SO_ACCEPTCONN, &opt, &ol) != 0 || opt != 1) return 36;
    if (getsockopt(srv, SOL_SOCKET, SO_TYPE, &opt, &ol) != 0 || opt != SOCK_STREAM) return 37;

    close(srv);

    /* 4. datagrams keep their edges */
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0) return 38;
    if (send(sv[0], "one", 3, 0) != 3 || send(sv[0], "second", 6, 0) != 6) return 39;
    if (recv(sv[1], buf, sizeof(buf), 0) != 3 || memcmp(buf, "one", 3)) return 40;

    /* a buffer too small cuts the message and says so */
    struct iovec iov = { buf, 3 };
    struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1 };

    if (recvmsg(sv[1], &mh, 0) != 3 || !(mh.msg_flags & MSG_TRUNC)) return 41;
    if (recv(sv[1], buf, sizeof(buf), MSG_DONTWAIT) != -1 || errno != EAGAIN) return 42;

    /* peek leaves it, and MSG_TRUNC asks how long it was */
    send(sv[0], "peeked", 6, 0);
    if (recv(sv[1], buf, 2, MSG_PEEK) != 2) return 43;
    if (recv(sv[1], buf, 2, MSG_TRUNC) != 6) return 44;
    close(sv[0]);
    close(sv[1]);

    /* with names: who sent it is told */
    unlink(DGRAM_A);
    unlink(DGRAM_B);

    int da = socket(AF_UNIX, SOCK_DGRAM, 0), db = socket(AF_UNIX, SOCK_DGRAM, 0);
    struct sockaddr_un sa_a = path_addr(DGRAM_A, &len), sa_b = path_addr(DGRAM_B, &len);

    if (bind(da, (struct sockaddr *)&sa_a, len) != 0 || bind(db, (struct sockaddr *)&sa_b, len) != 0) return 45;
    if (sendto(da, "to b", 4, 0, (struct sockaddr *)&sa_b, len) != 4) return 46;

    fl = sizeof(from);
    if (recvfrom(db, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl) != 4 || strcmp(from.sun_path, DGRAM_A)) return 47;

    /* a connected one takes only its peer's, and can be disconnected */
    if (connect(da, (struct sockaddr *)&sa_b, len) != 0 || send(da, "x", 1, 0) != 1) return 48;

    struct sockaddr_un unspec = { .sun_family = AF_UNSPEC };

    if (connect(da, (struct sockaddr *)&unspec, sizeof(unspec)) != 0) return 49;
    if (send(da, "x", 1, 0) != -1 || errno != ENOTCONN) return 50;

    close(da);
    close(db);
    unlink(DGRAM_A);
    unlink(DGRAM_B);

    /* 5. seqpacket: messages, and an end */
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) return 51;
    if (send(sv[0], "ab", 2, 0) != 2 || send(sv[0], "cde", 3, 0) != 3) return 52;
    if (recv(sv[1], buf, sizeof(buf), 0) != 2 || recv(sv[1], buf, sizeof(buf), 0) != 3) return 53;

    close(sv[0]);
    if (recv(sv[1], buf, sizeof(buf), 0) != 0) return 54;
    close(sv[1]);

    /* 6. files through a socket: a memfd both sides map, and a pipe */
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return 55;

    int mfd = memfd_create("shared", 0);
    if (mfd < 0 || ftruncate(mfd, 4096) != 0) return 56;

    char *mine = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    if (mine == MAP_FAILED) return 57;

    int p[2];
    if (pipe(p) != 0) return 58;

    int send_set[2] = { mfd, p[1] };
    if (send_fds(sv[0], send_set, 2, "files") != 5) return 59;

    int got[4], mflags = 0;
    int n = recv_fds(sv[1], got, 4, buf, sizeof(buf), MSG_CMSG_CLOEXEC, &mflags, CMSG_SPACE(sizeof(int) * 8));

    if (n != 2 || strcmp(buf, "files") || (mflags & (MSG_CTRUNC | MSG_TRUNC))) return 60;
    if (!(fcntl(got[0], F_GETFD) & FD_CLOEXEC)) return 61;

    char *theirs = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, got[0], 0);
    if (theirs == MAP_FAILED) return 62;

    strcpy(mine, "written through one mapping");
    if (strcmp(theirs, "written through one mapping")) return 63; /* the same pages */

    if (write(got[1], "via pipe", 8) != 8 || read(p[0], buf, sizeof(buf)) != 8) return 64;

    close(got[0]);
    close(got[1]);

    /* more files than the buffer holds: the rest are closed and it says so */
    if (send_fds(sv[0], send_set, 2, "cut") != 3) return 65;
    n = recv_fds(sv[1], got, 4, buf, sizeof(buf), 0, &mflags, CMSG_LEN(sizeof(int))); /* room for one: the spacing would hold two */

    if (n != 1 || !(mflags & MSG_CTRUNC)) return 66;
    close(got[0]);

    /* a read() that has no room for files takes the text, and the files are dropped */
    if (send_fds(sv[0], send_set, 1, "plain") != 5 || read(sv[1], buf, sizeof(buf)) != 5) return 67;
    close(sv[0]);
    close(sv[1]);

    /* 7. who sent it */
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0) return 68;

    opt = 1;
    if (setsockopt(sv[1], SOL_SOCKET, SO_PASSCRED, &opt, sizeof(opt)) != 0) return 69;
    if (send(sv[0], "who", 3, 0) != 3) return 70;

    char control[CMSG_SPACE(sizeof(struct ucred))];

    iov = (struct iovec){ buf, sizeof(buf) };
    mh = (struct msghdr){ .msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control) };

    if (recvmsg(sv[1], &mh, 0) != 3) return 71;

    struct cmsghdr *c2 = CMSG_FIRSTHDR(&mh);
    struct ucred got_cred;

    if (!c2 || c2->cmsg_type != SCM_CREDENTIALS) return 72;

    memcpy(&got_cred, CMSG_DATA(c2), sizeof(got_cred));
    if (got_cred.pid != getpid() || got_cred.uid != getuid()) return 73;

    close(sv[0]);
    close(sv[1]);

    /* 8. a full buffer: a nonblocking sender stops, a reader makes room, poll says so */
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) != 0) return 74;

    char big[4096];
    memset(big, 'z', sizeof(big));

    long total = 0;
    ssize_t w;

    while ((w = write(sv[0], big, sizeof(big))) > 0) total += w;

    if (w != -1 || errno != EAGAIN || total < 64 * 1024) return 75;

    struct pollfd out = { sv[0], POLLOUT, 0 };

    if (poll(&out, 1, 0) != 0) return 76;

    int queued = 0;
    if (ioctl(sv[1], FIONREAD, &queued) != 0 || queued != total) return 77;

    if (read(sv[1], big, sizeof(big)) != sizeof(big)) return 78;

    /* a reader in another process frees the room: the writer's poll wakes */
    pid = fork();
    if (pid == 0) {
        usleep(50000);

        while (read(sv[1], big, sizeof(big)) > 0) {}

        _exit(0);
    }

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    out.revents = 0;
    if (poll(&out, 1, 5000) != 1 || !(out.revents & POLLOUT) || ms_since(&t0) < 30) return 79;

    close(sv[0]);
    close(sv[1]);
    if (waitpid(pid, &status, 0) != pid) return 80;

    /* 9. waiting: a timeout on a read, and MSG_WAITALL across two writes */
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return 81;

    struct timeval tv = { 0, 60000 };

    if (setsockopt(sv[1], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) return 82;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (recv(sv[1], buf, sizeof(buf), 0) != -1 || errno != EAGAIN || ms_since(&t0) < 50) return 83;

    tv = (struct timeval){ 0, 0 };
    setsockopt(sv[1], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    pid = fork();
    if (pid == 0) {
        write(sv[0], "abc", 3);
        usleep(60000);
        write(sv[0], "defg", 4);
        _exit(0);
    }

    if (recv(sv[1], buf, 7, MSG_WAITALL) != 7 || memcmp(buf, "abcdefg", 7)) return 84;
    if (waitpid(pid, &status, 0) != pid) return 85;

    /* sendmmsg, recvmmsg */
    close(sv[0]);
    close(sv[1]);
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0) return 86;

    struct iovec iovs[2] = { { "m1", 2 }, { "m22", 3 } };
    struct mmsghdr mm[2] = { { .msg_hdr = { .msg_iov = &iovs[0], .msg_iovlen = 1 } },
                             { .msg_hdr = { .msg_iov = &iovs[1], .msg_iovlen = 1 } } };

    if (sendmmsg(sv[0], mm, 2, 0) != 2 || mm[1].msg_len != 3) return 87;

    struct iovec riov[2] = { { buf, 16 }, { buf + 16, 16 } };
    struct mmsghdr rm[2] = { { .msg_hdr = { .msg_iov = &riov[0], .msg_iovlen = 1 } },
                             { .msg_hdr = { .msg_iov = &riov[1], .msg_iovlen = 1 } } };

    if (recvmmsg(sv[1], rm, 2, 0, NULL) != 2 || rm[0].msg_len != 2 || rm[1].msg_len != 3) return 88;

    close(sv[0]);
    close(sv[1]);

    /* sizes of the buffers: told back doubled, and not less than a floor */
    int s = socket(AF_UNIX, SOCK_STREAM, 0);

    opt = 100000;
    if (setsockopt(s, SOL_SOCKET, SO_SNDBUF, &opt, sizeof(opt)) != 0) return 89;

    ol = sizeof(opt);
    if (getsockopt(s, SOL_SOCKET, SO_SNDBUF, &opt, &ol) != 0 || opt != 200000) return 90;
    if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &(int){ 1 }, sizeof(int)) != 0) return 91;
    if (getsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, &ol) != 0 || opt != 1) return 92;
    if (getsockopt(s, SOL_SOCKET, 999, &opt, &ol) != -1 || errno != ENOPROTOOPT) return 93;

    /* not a socket, and not a family */
    int plain = open("/etc/motd", O_RDONLY);

    if (listen(plain, 1) != -1 || errno != ENOTSOCK) return 94;
    if (socket(200, SOCK_STREAM, 0) != -1 || errno != EAFNOSUPPORT) return 95;
    close(plain);
    close(s);

    return 0x5e;
}
