#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

/* the test runs without a stderr: the line that failed is the exit status */
static int failed_line;

#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "pty: line %d: %s (errno %d)\n", __LINE__, #c, errno); \
            failed_line = __LINE__;                                      \
            return 1;                                                    \
        }                                                                \
    } while (0)

/* a master and its slave, opened the way a terminal emulator does */
static int open_pair(int *master, int *slave) {
    int m = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    CHECK(m >= 0);
    CHECK(grantpt(m) == 0 && unlockpt(m) == 0);

    char name[32];
    CHECK(ptsname_r(m, name, sizeof(name)) == 0);
    CHECK(strncmp(name, "/dev/pts/", 9) == 0);

    int s = open(name, O_RDWR | O_NOCTTY);
    CHECK(s >= 0);

    *master = m;
    *slave = s;
    return 0;
}

/* what is typed at the master is read at the slave, echo comes back, and what the slave writes has its newline made CRLF */
static int test_lines(void) {
    int m, s;
    char buf[64];

    if (open_pair(&m, &s)) return 1;

    CHECK(write(m, "hi\n", 3) == 3);

    CHECK(read(s, buf, sizeof(buf)) == 3 && memcmp(buf, "hi\n", 3) == 0);
    CHECK(read(m, buf, sizeof(buf)) == 4 && memcmp(buf, "hi\r\n", 4) == 0); /* the echo */

    CHECK(write(s, "x\ny", 3) == 3);
    CHECK(read(m, buf, sizeof(buf)) == 4 && memcmp(buf, "x\r\ny", 4) == 0);

    /* nothing more: not waiting */
    int flags = fcntl(m, F_GETFL);
    fcntl(m, F_SETFL, flags | O_NONBLOCK);
    CHECK(read(m, buf, sizeof(buf)) == -1 && errno == EAGAIN);

    close(s);
    close(m);
    return 0;
}

/* termios and the window size are the slave's, through either end */
static int test_termios(void) {
    int m, s;
    char buf[64];
    struct termios t;
    struct winsize ws = { .ws_row = 30, .ws_col = 100 };

    if (open_pair(&m, &s)) return 1;

    CHECK(ioctl(m, TIOCSWINSZ, &ws) == 0);

    struct winsize got;
    CHECK(ioctl(s, TIOCGWINSZ, &got) == 0 && got.ws_row == 30 && got.ws_col == 100);

    /* raw, as a program that wants the keys */
    CHECK(tcgetattr(s, &t) == 0);
    CHECK(t.c_lflag & ICANON);
    cfmakeraw(&t);
    CHECK(tcsetattr(s, TCSANOW, &t) == 0);

    CHECK(tcgetattr(m, &t) == 0 && !(t.c_lflag & ICANON));

    CHECK(write(m, "ab", 2) == 2);
    CHECK(read(s, buf, sizeof(buf)) == 2 && memcmp(buf, "ab", 2) == 0);

    /* poll: the slave has nothing to say, then it does */
    struct pollfd p = { .fd = m, .events = POLLIN };
    CHECK(poll(&p, 1, 0) == 0);
    CHECK(write(s, "z", 1) == 1);
    CHECK(poll(&p, 1, 1000) == 1 && (p.revents & POLLIN));

    close(s);
    close(m);
    return 0;
}

/* a slave is locked until unlockpt() */
static int test_locked(void) {
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    CHECK(m >= 0);

    char name[32];
    CHECK(ptsname_r(m, name, sizeof(name)) == 0);
    CHECK(open(name, O_RDWR | O_NOCTTY) == -1 && errno == EIO);

    CHECK(unlockpt(m) == 0);

    /* not as the terminal of this session: closing the master would hang the test up */
    int s = open(name, O_RDWR | O_NOCTTY);
    CHECK(s >= 0);

    close(s);
    close(m);
    return 0;
}

/* a child on the slave, as a terminal emulator starts a shell: its own session, the slave its terminal and its 0, 1, 2.
 * when it is gone the master reads the end as EIO */
static int test_child(void) {
    int m, s;
    char buf[64], name[32];

    if (open_pair(&m, &s)) return 1;

    CHECK(ptsname_r(m, name, sizeof(name)) == 0);
    close(s); /* the child's is the only one */

    pid_t pid = fork();
    CHECK(pid >= 0);

    if (pid == 0) {
        close(m);

        if (setsid() < 0) _exit(1);

        int fd = open(name, O_RDWR);
        if (fd < 0 || ioctl(fd, TIOCSCTTY, 0) < 0) _exit(2);

        dup2(fd, 0);
        dup2(fd, 1);
        dup2(fd, 2);

        /* /dev/tty is the slave, not the console, and the child's group is its foreground */
        int t = open("/dev/tty", O_RDWR);
        if (t < 0 || tcgetpgrp(t) != getpgrp()) _exit(4);

        if (write(t, "child\n", 6) != 6) _exit(3);
        _exit(0);
    }

    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);

    CHECK(read(m, buf, sizeof(buf)) == 7 && memcmp(buf, "child\r\n", 7) == 0);
    CHECK(read(m, buf, sizeof(buf)) == -1 && errno == EIO);

    struct pollfd p = { .fd = m, .events = POLLIN };
    CHECK(poll(&p, 1, 0) == 1 && (p.revents & POLLHUP));

    close(m);
    return 0;
}

/* the master gone: the slave reads the end */
static int test_hangup(void) {
    int m, s;
    char buf[8];

    if (open_pair(&m, &s)) return 1;

    close(m);

    CHECK(read(s, buf, sizeof(buf)) == 0);

    struct pollfd p = { .fd = s, .events = POLLIN };
    CHECK(poll(&p, 1, 0) == 1 && (p.revents & POLLHUP));

    close(s);
    return 0;
}

int main(void) {
    int (*tests[])(void) = { test_lines, test_termios, test_locked, test_child, test_hangup };
    const char *names[] = { "lines", "termios", "locked", "child", "hangup" };

    for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        if (tests[i]()) {
            fprintf(stderr, "pty: %s failed\n", names[i]);
            return failed_line;
        }
    }

    return 0x5e;
}
