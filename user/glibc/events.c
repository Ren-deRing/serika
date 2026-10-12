#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LOCKFILE "/tmp/events.lock"

#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "events: line %d: %s\n", __LINE__, #c);      \
            return 1;                                                    \
        }                                                                \
    } while (0)

static uint64_t now_ms(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000;
}

static void sleep_ms(long ms) {
    struct timespec ts = { ms / 1000, ms % 1000 * 1000000 };

    nanosleep(&ts, NULL);
}

static int wait_child(pid_t pid) {
    int status;

    return waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int test_eventfd(void) {
    uint64_t v;

    int e = eventfd(0, EFD_NONBLOCK);
    CHECK(e >= 0);
    CHECK(read(e, &v, 8) == -1 && errno == EAGAIN);

    v = 3;
    CHECK(write(e, &v, 8) == 8);
    v = 4;
    CHECK(write(e, &v, 8) == 8);

    struct pollfd p = { e, POLLIN | POLLOUT, 0 };
    CHECK(poll(&p, 1, 0) == 1 && p.revents == (POLLIN | POLLOUT));

    CHECK(read(e, &v, 8) == 8 && v == 7);
    CHECK(read(e, &v, 8) == -1 && errno == EAGAIN);
    CHECK(poll(&p, 1, 0) == 1 && p.revents == POLLOUT);

    char small[4];
    CHECK(read(e, small, 4) == -1 && errno == EINVAL);
    CHECK(write(e, small, 4) == -1 && errno == EINVAL);

    v = ~0ULL;
    CHECK(write(e, &v, 8) == -1 && errno == EINVAL);

    /* as full as it goes */
    v = ~0ULL - 1;
    CHECK(write(e, &v, 8) == 8);
    v = 1;
    CHECK(write(e, &v, 8) == -1 && errno == EAGAIN);
    CHECK(poll(&p, 1, 0) == 1 && p.revents == POLLIN);
    CHECK(read(e, &v, 8) == 8 && v == ~0ULL - 1);
    close(e);

    /* a semaphore gives one at a time */
    e = eventfd(3, EFD_SEMAPHORE | EFD_NONBLOCK);
    CHECK(e >= 0);

    for (int i = 0; i < 3; i++) CHECK(read(e, &v, 8) == 8 && v == 1);

    CHECK(read(e, &v, 8) == -1 && errno == EAGAIN);
    close(e);

    CHECK(eventfd(0, 1 << 20) == -1 && errno == EINVAL);

    /* a reader that waits, and one that writes later */
    e = eventfd(0, 0);

    pid_t pid = fork();
    if (pid == 0) {
        sleep_ms(50);
        v = 9;
        _exit(write(e, &v, 8) == 8 ? 0 : 1);
    }

    uint64_t t0 = now_ms();

    CHECK(read(e, &v, 8) == 8 && v == 9);
    CHECK(now_ms() - t0 >= 30);
    CHECK(wait_child(pid));
    close(e);

    return 0;
}

static int test_epoll_pipe(void) {
    struct epoll_event ev, out[4];
    int fds[2];

    int ep = epoll_create1(EPOLL_CLOEXEC);
    CHECK(ep >= 0);
    CHECK(epoll_create(0) == -1 && errno == EINVAL);
    CHECK(epoll_create1(1) == -1 && errno == EINVAL);
    CHECK(pipe(fds) == 0);

    ev = (struct epoll_event){ .events = EPOLLIN, .data.u64 = 42 };
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &ev) == 0);
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &ev) == -1 && errno == EEXIST);
    CHECK(epoll_wait(ep, out, 4, 0) == 0);

    CHECK(write(fds[1], "x", 1) == 1);

    /* level triggered: told until it is read */
    for (int i = 0; i < 2; i++) {
        CHECK(epoll_wait(ep, out, 4, 0) == 1);
        CHECK(out[0].events == EPOLLIN && out[0].data.u64 == 42);
    }

    char c;
    CHECK(read(fds[0], &c, 1) == 1);
    CHECK(epoll_wait(ep, out, 4, 0) == 0);

    /* ONESHOT: told once, then off until a MOD */
    ev = (struct epoll_event){ .events = EPOLLIN | EPOLLONESHOT, .data.u64 = 7 };
    CHECK(epoll_ctl(ep, EPOLL_CTL_MOD, fds[0], &ev) == 0);
    CHECK(write(fds[1], "y", 1) == 1);
    CHECK(epoll_wait(ep, out, 4, 0) == 1 && out[0].data.u64 == 7);
    CHECK(epoll_wait(ep, out, 4, 0) == 0);
    CHECK(epoll_ctl(ep, EPOLL_CTL_MOD, fds[0], &ev) == 0);
    CHECK(epoll_wait(ep, out, 4, 0) == 1);

    /* the end of the pipe that is written: ready for it, and told when the reader is gone */
    CHECK(read(fds[0], &c, 1) == 1);
    ev = (struct epoll_event){ .events = EPOLLOUT, .data.u64 = 8 };
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, fds[1], &ev) == 0);
    CHECK(epoll_wait(ep, out, 4, 0) == 1 && out[0].events == EPOLLOUT && out[0].data.u64 == 8);
    CHECK(epoll_ctl(ep, EPOLL_CTL_DEL, fds[1], NULL) == 0);
    CHECK(epoll_ctl(ep, EPOLL_CTL_DEL, fds[1], NULL) == -1 && errno == ENOENT);
    CHECK(epoll_ctl(ep, EPOLL_CTL_MOD, fds[1], &ev) == -1 && errno == ENOENT);

    ev = (struct epoll_event){ .events = EPOLLIN, .data.u64 = 9 };
    CHECK(epoll_ctl(ep, EPOLL_CTL_MOD, fds[0], &ev) == 0);
    close(fds[1]);
    CHECK(epoll_wait(ep, out, 4, 0) == 1 && (out[0].events & EPOLLHUP) && out[0].data.u64 == 9);

    close(fds[0]);
    close(ep);
    return 0;
}

static int test_epoll_errors(void) {
    struct epoll_event ev = { .events = EPOLLIN }, out[1];
    int fds[2];

    int ep = epoll_create1(0);

    CHECK(pipe(fds) == 0);

    /* things that cannot be listed, and things that are not there */
    int file = open(LOCKFILE, O_RDWR | O_CREAT, 0644);
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, file, &ev) == -1 && errno == EPERM);
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, 200, &ev) == -1 && errno == EBADF);
    CHECK(epoll_ctl(200, EPOLL_CTL_ADD, fds[0], &ev) == -1 && errno == EBADF);
    CHECK(epoll_ctl(fds[0], EPOLL_CTL_ADD, fds[1], &ev) == -1 && errno == EINVAL);
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, ep, &ev) == -1 && errno == EINVAL);
    CHECK(epoll_ctl(ep, 99, fds[0], &ev) == -1 && errno == EINVAL);
    CHECK(epoll_wait(ep, out, 0, 0) == -1 && errno == EINVAL);
    CHECK(epoll_wait(fds[0], out, 1, 0) == -1 && errno == EINVAL);
    CHECK(epoll_wait(200, out, 1, 0) == -1 && errno == EBADF);
    close(file);
    unlink(LOCKFILE);

    /* a file closed while listed is gone from it, and its number can be listed again */
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &ev) == 0);
    close(fds[0]);
    close(fds[1]);
    CHECK(epoll_wait(ep, out, 1, 0) == 0);

    CHECK(pipe(fds) == 0);
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &ev) == 0);
    CHECK(write(fds[1], "z", 1) == 1);
    CHECK(epoll_wait(ep, out, 1, 0) == 1);

    close(fds[0]);
    close(fds[1]);
    close(ep);
    return 0;
}

static volatile int usr1_seen;

static void on_usr1(int sig) {
    (void)sig;
    usr1_seen++;
}

static int test_epoll_wait(void) {
    struct epoll_event ev, out[4];
    int a[2], b[2];

    int ep = epoll_create1(0);

    CHECK(pipe(a) == 0 && pipe(b) == 0);

    ev = (struct epoll_event){ .events = EPOLLIN, .data.u64 = 1 };
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, a[0], &ev) == 0);
    ev.data.u64 = 2;
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, b[0], &ev) == 0);

    /* nothing: the time given goes by */
    uint64_t t0 = now_ms();

    CHECK(epoll_wait(ep, out, 4, 60) == 0);
    CHECK(now_ms() - t0 >= 50);

    /* asleep until a child writes */
    pid_t pid = fork();
    if (pid == 0) {
        sleep_ms(50);
        _exit(write(b[1], "w", 1) == 1 ? 0 : 1);
    }

    t0 = now_ms();

    CHECK(epoll_wait(ep, out, 4, -1) == 1 && out[0].data.u64 == 2);
    CHECK(now_ms() - t0 >= 30);
    CHECK(wait_child(pid));

    /* both ready, one place: they take turns */
    CHECK(write(a[1], "w", 1) == 1);
    CHECK(epoll_wait(ep, out, 4, 0) == 2);

    uint64_t first, second;
    CHECK(epoll_wait(ep, out, 1, 0) == 1);
    first = out[0].data.u64;
    CHECK(epoll_wait(ep, out, 1, 0) == 1);
    second = out[0].data.u64;
    CHECK(first != second);

    /* a signal ends the wait: EINTR */
    char c;
    CHECK(read(a[0], &c, 1) == 1 && read(b[0], &c, 1) == 1);

    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);
    CHECK(sigprocmask(SIG_BLOCK, &block, &old) == 0);

    pid_t parent = getpid();

    pid = fork();
    if (pid == 0) {
        sleep_ms(50);
        _exit(kill(parent, SIGUSR1));
    }

    /* the mask of the call lets it in; the handler runs before the wait is over, and blocked again after */
    struct sigaction sa = { .sa_handler = on_usr1 };
    sigaction(SIGUSR1, &sa, NULL);

    sigset_t none;
    sigemptyset(&none);
    CHECK(epoll_pwait(ep, out, 4, 2000, &none) == -1 && errno == EINTR);
    CHECK(usr1_seen == 1);
    CHECK(wait_child(pid));

    sigset_t now;
    CHECK(sigprocmask(SIG_BLOCK, NULL, &now) == 0 && sigismember(&now, SIGUSR1));

    sigprocmask(SIG_SETMASK, &old, NULL);

    close(a[0]); close(a[1]); close(b[0]); close(b[1]);
    close(ep);
    return 0;
}

static int test_epoll_socket(void) {
    struct epoll_event ev, out[2];
    int sv[2];

    int ep = epoll_create1(0);

    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    ev = (struct epoll_event){ .events = EPOLLIN | EPOLLOUT | EPOLLRDHUP, .data.u64 = 5 };
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, sv[0], &ev) == 0);

    CHECK(epoll_wait(ep, out, 2, 0) == 1 && out[0].events == EPOLLOUT);

    CHECK(write(sv[1], "hi", 2) == 2);
    CHECK(epoll_wait(ep, out, 2, 0) == 1 && out[0].events == (EPOLLIN | EPOLLOUT));

    close(sv[1]);
    CHECK(epoll_wait(ep, out, 2, 0) == 1);
    CHECK((out[0].events & EPOLLIN) && (out[0].events & EPOLLRDHUP) && (out[0].events & EPOLLHUP));

    /* the other end listed too, and closed while it is: it just goes */
    close(sv[0]);
    CHECK(epoll_wait(ep, out, 2, 0) == 0);

    close(ep);
    return 0;
}

static int test_epoll_nested(void) {
    struct epoll_event ev = { .events = EPOLLIN }, out[2];
    int fds[2];

    int inner = epoll_create1(0), outer = epoll_create1(0);

    CHECK(pipe(fds) == 0);
    CHECK(epoll_ctl(inner, EPOLL_CTL_ADD, fds[0], &ev) == 0);
    CHECK(epoll_ctl(outer, EPOLL_CTL_ADD, inner, &ev) == 0);

    /* no loop: the outer one cannot go in the inner */
    CHECK(epoll_ctl(inner, EPOLL_CTL_ADD, outer, &ev) == -1 && errno == ELOOP);

    CHECK(epoll_wait(outer, out, 2, 0) == 0);

    /* an epoll is a file that poll() takes, and wakes when what is in it is ready */
    struct pollfd p = { outer, POLLIN, 0 };

    pid_t pid = fork();
    if (pid == 0) {
        sleep_ms(50);
        _exit(write(fds[1], "n", 1) == 1 ? 0 : 1);
    }

    uint64_t t0 = now_ms();

    CHECK(poll(&p, 1, 2000) == 1 && p.revents == POLLIN);
    CHECK(now_ms() - t0 >= 30);
    CHECK(wait_child(pid));

    CHECK(epoll_wait(outer, out, 2, 0) == 1 && out[0].events == EPOLLIN);

    close(fds[0]); close(fds[1]);
    close(inner);
    close(outer);
    return 0;
}

static int test_timerfd(void) {
    struct itimerspec its, cur, old;
    uint64_t n;

    CHECK(timerfd_create(99, 0) == -1 && errno == EINVAL);

    int t = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    CHECK(t >= 0);
    CHECK(read(t, &n, 8) == -1 && errno == EAGAIN);
    CHECK(timerfd_gettime(t, &cur) == 0 && cur.it_value.tv_sec == 0 && cur.it_value.tv_nsec == 0);

    /* once, in 30 ms */
    its = (struct itimerspec){ .it_value = { 0, 30000000 } };
    CHECK(timerfd_settime(t, 0, &its, NULL) == 0);
    CHECK(timerfd_gettime(t, &cur) == 0 && cur.it_value.tv_sec == 0 && cur.it_value.tv_nsec > 0);

    struct pollfd p = { t, POLLIN, 0 };
    uint64_t t0 = now_ms();

    CHECK(poll(&p, 1, 0) == 0);
    CHECK(poll(&p, 1, 2000) == 1);
    CHECK(now_ms() - t0 >= 20);
    CHECK(read(t, &n, 8) == 8 && n == 1);
    CHECK(read(t, &n, 8) == -1 && errno == EAGAIN);
    CHECK(timerfd_gettime(t, &cur) == 0 && cur.it_value.tv_sec == 0 && cur.it_value.tv_nsec == 0);

    /* every 20 ms: the ones that went by are counted */
    its = (struct itimerspec){ .it_interval = { 0, 20000000 }, .it_value = { 0, 20000000 } };
    CHECK(timerfd_settime(t, 0, &its, NULL) == 0);
    sleep_ms(110);
    CHECK(read(t, &n, 8) == 8 && n >= 3 && n <= 6);
    CHECK(timerfd_gettime(t, &cur) == 0 && cur.it_interval.tv_nsec == 20000000);
    CHECK(cur.it_value.tv_sec == 0 && cur.it_value.tv_nsec > 0 && cur.it_value.tv_nsec <= 20000000);

    /* and it keeps going: another is there to wait for */
    CHECK(poll(&p, 1, 2000) == 1);
    CHECK(read(t, &n, 8) == 8 && n >= 1);

    /* off: what was due is not told, and nothing comes. the old setting comes back */
    its = (struct itimerspec){ 0 };
    CHECK(timerfd_settime(t, 0, &its, &old) == 0 && old.it_interval.tv_nsec == 20000000);
    sleep_ms(50);
    CHECK(read(t, &n, 8) == -1 && errno == EAGAIN);

    /* a point in time */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    its = (struct itimerspec){ .it_value = { now.tv_sec, now.tv_nsec + 30000000 } };
    if (its.it_value.tv_nsec >= 1000000000) {
        its.it_value.tv_sec++;
        its.it_value.tv_nsec -= 1000000000;
    }
    CHECK(timerfd_settime(t, TFD_TIMER_ABSTIME, &its, NULL) == 0);
    CHECK(poll(&p, 1, 2000) == 1);
    CHECK(read(t, &n, 8) == 8 && n == 1);

    /* in the past: at once */
    its = (struct itimerspec){ .it_value = { 1, 0 } };
    CHECK(timerfd_settime(t, TFD_TIMER_ABSTIME, &its, NULL) == 0);
    CHECK(poll(&p, 1, 2000) == 1);
    CHECK(read(t, &n, 8) == 8 && n == 1);

    /* the real time clock, absolute */
    clock_gettime(CLOCK_REALTIME, &now);
    int rt = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK);
    its = (struct itimerspec){ .it_value = { now.tv_sec + 1, now.tv_nsec } };
    CHECK(timerfd_settime(rt, TFD_TIMER_ABSTIME, &its, NULL) == 0);
    CHECK(read(rt, &n, 8) == -1 && errno == EAGAIN);

    struct pollfd rp = { rt, POLLIN, 0 };
    CHECK(poll(&rp, 1, 3000) == 1);
    CHECK(read(rt, &n, 8) == 8 && n == 1);
    close(rt);

    /* a new setting counts from zero, and it is a file for the one that waits in a read */
    close(t);
    t = timerfd_create(CLOCK_MONOTONIC, 0);
    its = (struct itimerspec){ .it_value = { 0, 40000000 } };
    CHECK(timerfd_settime(t, 0, &its, NULL) == 0);

    t0 = now_ms();
    CHECK(read(t, &n, 8) == 8 && n == 1);
    CHECK(now_ms() - t0 >= 25);

    /* not a timerfd, and bad times */
    int e = eventfd(0, 0);
    CHECK(timerfd_settime(e, 0, &its, NULL) == -1 && errno == EINVAL);
    CHECK(timerfd_gettime(e, &cur) == -1 && errno == EINVAL);
    CHECK(timerfd_settime(200, 0, &its, NULL) == -1 && errno == EBADF);

    its.it_value.tv_nsec = 1000000000;
    CHECK(timerfd_settime(t, 0, &its, NULL) == -1 && errno == EINVAL);

    close(e);
    close(t);
    return 0;
}

static int test_signalfd(void) {
    struct signalfd_siginfo si[2];
    sigset_t mask;

    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    sigaddset(&mask, SIGUSR2);
    CHECK(sigprocmask(SIG_BLOCK, &mask, NULL) == 0);

    int s = signalfd(-1, &mask, SFD_NONBLOCK);
    CHECK(s >= 0);
    CHECK(read(s, si, sizeof(si[0])) == -1 && errno == EAGAIN);
    CHECK(read(s, si, 10) == -1 && errno == EINVAL);

    struct pollfd p = { s, POLLIN, 0 };
    CHECK(poll(&p, 1, 0) == 0);

    CHECK(kill(getpid(), SIGUSR1) == 0);
    CHECK(poll(&p, 1, 0) == 1);

    memset(si, 0, sizeof(si));
    CHECK(read(s, si, sizeof(si[0])) == sizeof(si[0]));
    CHECK(si[0].ssi_signo == SIGUSR1 && si[0].ssi_pid == (uint32_t)getpid());
    CHECK(read(s, si, sizeof(si[0])) == -1 && errno == EAGAIN);

    /* two at once, in one read, the lower first */
    CHECK(kill(getpid(), SIGUSR2) == 0 && kill(getpid(), SIGUSR1) == 0);
    CHECK(read(s, si, sizeof(si)) == sizeof(si));
    CHECK(si[0].ssi_signo == SIGUSR1 && si[1].ssi_signo == SIGUSR2);

    /* one that is not in the mask stays pending, for sigpending() */
    sigset_t other, pending;
    sigemptyset(&other);
    sigaddset(&other, SIGURG);
    sigprocmask(SIG_BLOCK, &other, NULL);
    kill(getpid(), SIGURG);
    CHECK(poll(&p, 1, 0) == 0);
    CHECK(sigpending(&pending) == 0 && sigismember(&pending, SIGURG));

    /* a signalfd of epoll's */
    int ep = epoll_create1(0);
    struct epoll_event ev = { .events = EPOLLIN, .data.u64 = 3 }, out[1];

    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, s, &ev) == 0);
    CHECK(epoll_wait(ep, out, 1, 0) == 0);
    CHECK(kill(getpid(), SIGUSR2) == 0);
    CHECK(epoll_wait(ep, out, 1, 0) == 1 && out[0].data.u64 == 3);
    CHECK(read(s, si, sizeof(si[0])) == sizeof(si[0]) && si[0].ssi_signo == SIGUSR2);

    /* the mask can be changed: SIGUSR2 is not wanted now */
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    CHECK(signalfd(s, &mask, 0) == s);
    CHECK(kill(getpid(), SIGUSR2) == 0);
    CHECK(read(s, si, sizeof(si[0])) == -1 && errno == EAGAIN);

    int e = eventfd(0, 0);
    CHECK(signalfd(e, &mask, 0) == -1 && errno == EINVAL);
    CHECK(signalfd(-1, &mask, 1 << 20) == -1 && errno == EINVAL);
    close(e);
    close(ep);
    close(s);

    /* asleep in a read until another process sends one */
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    s = signalfd(-1, &mask, 0);

    pid_t parent = getpid();
    pid_t pid = fork();

    if (pid == 0) {
        sleep_ms(50);
        _exit(kill(parent, SIGUSR1));
    }

    uint64_t t0 = now_ms();

    CHECK(read(s, si, sizeof(si[0])) == sizeof(si[0]) && si[0].ssi_signo == SIGUSR1 && si[0].ssi_pid == (uint32_t)pid);
    CHECK(now_ms() - t0 >= 30);
    CHECK(wait_child(pid));

    close(s);
    return 0;
}

static int test_flock(void) {
    int a = open(LOCKFILE, O_RDWR | O_CREAT, 0644);
    int b = open(LOCKFILE, O_RDWR);

    CHECK(a >= 0 && b >= 0);

    /* two openings of one file are two owners */
    CHECK(flock(a, LOCK_EX) == 0);
    CHECK(flock(b, LOCK_EX | LOCK_NB) == -1 && errno == EWOULDBLOCK);
    CHECK(flock(b, LOCK_SH | LOCK_NB) == -1 && errno == EWOULDBLOCK);
    CHECK(flock(a, LOCK_UN) == 0);
    CHECK(flock(b, LOCK_EX | LOCK_NB) == 0);
    CHECK(flock(a, LOCK_SH | LOCK_NB) == -1 && errno == EWOULDBLOCK);

    /* shared with shared, and the change of one */
    CHECK(flock(b, LOCK_SH) == 0);
    CHECK(flock(a, LOCK_SH | LOCK_NB) == 0);
    CHECK(flock(a, LOCK_EX | LOCK_NB) == -1 && errno == EWOULDBLOCK);

    /* closing the file lets the lock go */
    close(b);
    CHECK(flock(a, LOCK_EX | LOCK_NB) == 0);

    /* a dup is the same owner: the unlock of one is the unlock of both */
    int c = dup(a);
    b = open(LOCKFILE, O_RDWR);
    CHECK(flock(c, LOCK_EX | LOCK_NB) == 0);
    close(a);
    CHECK(flock(b, LOCK_EX | LOCK_NB) == -1 && errno == EWOULDBLOCK);
    CHECK(flock(c, LOCK_UN) == 0);
    CHECK(flock(b, LOCK_EX | LOCK_NB) == 0);
    CHECK(flock(b, LOCK_UN) == 0);

    CHECK(flock(b, 0) == -1 && errno == EINVAL);
    CHECK(flock(b, LOCK_SH | LOCK_EX) == -1 && errno == EINVAL);
    CHECK(flock(200, LOCK_SH) == -1 && errno == EBADF);

    /* a process that waits for it */
    CHECK(flock(b, LOCK_EX) == 0);

    pid_t pid = fork();
    if (pid == 0) {
        int d = open(LOCKFILE, O_RDWR);

        if (flock(d, LOCK_EX | LOCK_NB) == 0) _exit(2);

        _exit(flock(d, LOCK_EX) == 0 ? 0 : 1);
    }

    sleep_ms(50);

    /* the child has the same file open as we have: its lock is its own. it is still asleep */
    int status;
    CHECK(waitpid(pid, &status, WNOHANG) == 0);
    CHECK(flock(b, LOCK_UN) == 0);
    CHECK(wait_child(pid));

    close(b);
    close(c);
    unlink(LOCKFILE);
    return 0;
}

int main(void) {
    int (*tests[])(void) = { test_eventfd, test_epoll_pipe, test_epoll_errors, test_epoll_wait, test_epoll_socket,
                             test_epoll_nested, test_timerfd, test_signalfd, test_flock };
    const char *names[] = { "eventfd", "epoll pipe", "epoll errors", "epoll wait", "epoll socket", "epoll nested",
                            "timerfd", "signalfd", "flock" };

    for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        if (tests[i]()) {
            fprintf(stderr, "events: %s failed\n", names[i]);
            return 1;
        }
    }

    return 0x5e;
}
