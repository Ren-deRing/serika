#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/select.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile int alarms;

static void on_alarm(int sig) {
    (void)sig;
    alarms++;
}

static void *die_holding(void *arg) {
    pthread_mutex_lock(arg);
    return NULL; /* and the thread ends with it */
}

static long ms_since(const struct timespec *t0) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);

    return (t.tv_sec - t0->tv_sec) * 1000 + (t.tv_nsec - t0->tv_nsec) / 1000000;
}

/* the exit code is the number of the check that failed */
int main(void) {
    unsigned char a[32], b[32];

    /* getrandom */
    if (getrandom(a, sizeof(a), 0) != sizeof(a) || getrandom(b, sizeof(b), 0) != sizeof(b)) return 1;
    if (!memcmp(a, b, sizeof(a))) return 2;

    /* madvise: the pages go and read zero. hints are taken, a bad range is not */
    char *m = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) return 3;

    memset(m, 0x5a, 8192);
    if (madvise(m, 8192, MADV_DONTNEED) != 0) return 4;
    if (m[0] != 0 || m[8191] != 0) return 5;

    m[0] = 1;
    if (madvise(m, 8192, MADV_HUGEPAGE) != 0 || m[0] != 1) return 6;
    if (madvise(m + 1, 10, MADV_DONTNEED) == 0 || errno != EINVAL) return 7;

    /* mremap: growing, moving if it has to, keeps the data. shrinking gives the tail back */
    char *r = mmap(NULL, 4 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (r == MAP_FAILED) return 38;

    for (int i = 0; i < 4; i++) r[i * 4096] = i + 1;

    char *big = mremap(r, 4 * 4096, 64 * 4096, MREMAP_MAYMOVE);
    if (big == MAP_FAILED) return 39;

    for (int i = 0; i < 4; i++)
        if (big[i * 4096] != i + 1) return 40;

    if (big[40 * 4096] != 0) return 41;
    big[63 * 4096] = 7;

    if (mremap(big, 64 * 4096, 2 * 4096, 0) != big || big[4096] != 2) return 42;

    /* poll on a pipe: nothing, data, a writer's room, the end */
    int p[2];
    char c;
    struct timespec t0;

    if (pipe(p) != 0) return 8;

    struct pollfd pr = { .fd = p[0], .events = POLLIN };
    struct pollfd pw = { .fd = p[1], .events = POLLOUT };

    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (poll(&pr, 1, 30) != 0 || ms_since(&t0) < 25) return 9;

    if (write(p[1], "x", 1) != 1) return 10;
    if (poll(&pr, 1, 1000) != 1 || !(pr.revents & POLLIN)) return 11;
    if (poll(&pw, 1, 1000) != 1 || !(pw.revents & POLLOUT)) return 12;

    if (read(p[0], &c, 1) != 1) return 13;
    close(p[1]);

    if (poll(&pr, 1, 1000) != 1 || !(pr.revents & POLLHUP)) return 14;
    close(p[0]);

    /* a bad fd, and a sleep with nothing to watch */
    struct pollfd bad = { .fd = 99, .events = POLLIN };
    if (poll(&bad, 1, 0) != 1 || !(bad.revents & POLLNVAL)) return 15;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (poll(NULL, 0, 30) != 0 || ms_since(&t0) < 25) return 16;

    /* select */
    int q[2];
    fd_set rs;
    struct timeval tv = { 0, 20000 };

    if (pipe(q) != 0) return 17;

    FD_ZERO(&rs);
    FD_SET(q[0], &rs);
    if (select(q[0] + 1, &rs, NULL, NULL, &tv) != 0 || FD_ISSET(q[0], &rs)) return 18;

    write(q[1], "y", 1);
    FD_ZERO(&rs);
    FD_SET(q[0], &rs);
    if (select(q[0] + 1, &rs, NULL, NULL, NULL) != 1 || !FD_ISSET(q[0], &rs)) return 19;
    read(q[0], &c, 1);

    /* a poll asleep, woken by another process writing */
    pid_t pid = fork();
    if (pid < 0) return 20;

    if (pid == 0) {
        usleep(50000);
        write(q[1], "z", 1);
        _exit(0);
    }

    pr.fd = q[0];
    pr.revents = 0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (poll(&pr, 1, 5000) != 1 || ms_since(&t0) < 40 || ms_since(&t0) > 2000) return 21;

    int status;
    if (waitpid(pid, &status, 0) != pid) return 22;

    /* sendfile: a file into the pipe */
    int fd = open("/etc/motd", O_RDONLY);
    struct stat st;

    if (fd < 0 || fstat(fd, &st) != 0 || st.st_size == 0) return 23;

    char want[64], got[64];
    size_t n = st.st_size < (off_t)sizeof(want) ? (size_t)st.st_size : sizeof(want);

    read(q[0], &c, 1); /* the z */
    if (sendfile(q[1], fd, NULL, n) != (ssize_t)n) return 24;
    if (pread(fd, want, n, 0) != (ssize_t)n || read(q[0], got, n) != (ssize_t)n || memcmp(want, got, n)) return 25;

    off_t off = 2;
    if (n > 3 && (sendfile(q[1], fd, &off, 1) != 1 || off != 3)) return 26;
    close(fd);

    /* process groups: a child of its own, signalled and waited for through it */
    pid = fork();
    if (pid < 0) return 29;

    if (pid == 0) {
        if (setpgid(0, 0) != 0 || getpgrp() != getpid()) _exit(1);

        pause();
        _exit(2);
    }

    if (getpgid(pid) != getpgrp() && getpgid(pid) != pid) return 30; /* before or after the child's own setpgid */
    if (setpgid(pid, pid) != 0 || getpgid(pid) != pid) return 31;
    if (getpgrp() == pid) return 32;

    if (kill(-pid, SIGTERM) != 0) {
        printf("syscalls: kill(-%d) failed -- errno %d\n", pid, errno);
        return 33;
    }

    if (waitpid(-pid, &status, 0) != pid || !WIFSIGNALED(status) || WTERMSIG(status) != SIGTERM) return 34;
    if (kill(-pid, 0) == 0 || errno != ESRCH) return 35;

    /* a session of its own is not for a group's leader */
    pid = fork();
    if (pid < 0) return 36;

    if (pid == 0) _exit(setsid() == getpid() && getsid(0) == getpid() ? 0 : 1);

    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return 37;

    /* O_NONBLOCK: a pipe with nothing in it says so instead of waiting, and the flag can be set and cleared */
    int nb[2];
    if (pipe2(nb, O_NONBLOCK) != 0) return 43;
    if (read(nb[0], &c, 1) != -1 || errno != EAGAIN) return 44;
    if (!(fcntl(nb[0], F_GETFL) & O_NONBLOCK)) return 45;
    if (fcntl(nb[0], F_SETFL, 0) != 0 || (fcntl(nb[0], F_GETFL) & O_NONBLOCK)) return 46;
    close(nb[0]);
    close(nb[1]);

    /* a named pipe: both ends open without waiting, and the data comes through */
    unlink("/tmp/syscalls.fifo");
    if (mkfifo("/tmp/syscalls.fifo", 0600) != 0) return 47;

    struct stat fst;
    if (stat("/tmp/syscalls.fifo", &fst) != 0 || !S_ISFIFO(fst.st_mode)) return 48;

    int fr = open("/tmp/syscalls.fifo", O_RDONLY | O_NONBLOCK);
    int fw = open("/tmp/syscalls.fifo", O_WRONLY | O_NONBLOCK);
    if (fr < 0 || fw < 0) return 49;
    if (write(fw, "ok", 2) != 2 || read(fr, got, 2) != 2 || memcmp(got, "ok", 2)) return 50;
    close(fr);
    close(fw);

    /* and with nobody on the other side yet, a blocking open waits for it */
    pid = fork();
    if (pid < 0) return 51;

    if (pid == 0) {
        int w = open("/tmp/syscalls.fifo", O_WRONLY); /* waits for the reader below */
        _exit(w >= 0 && write(w, "hi", 2) == 2 ? 0 : 1);
    }

    fr = open("/tmp/syscalls.fifo", O_RDONLY); /* and this for the writer */
    if (fr < 0 || read(fr, got, 2) != 2 || memcmp(got, "hi", 2)) return 52;
    close(fr);

    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return 53;
    unlink("/tmp/syscalls.fifo");

    /* the cpus there are */
    cpu_set_t cpus;
    if (sched_getaffinity(0, sizeof(cpus), &cpus) != 0 || CPU_COUNT(&cpus) < 1) return 54;
    if (sched_setaffinity(0, sizeof(cpus), &cpus) != 0) return 55;

    /* a thread that ends holding a robust mutex leaves it owner-died */
    pthread_mutexattr_t at;
    pthread_mutex_t mu;
    pthread_t t;

    pthread_mutexattr_init(&at);
    pthread_mutexattr_setrobust(&at, PTHREAD_MUTEX_ROBUST);
    pthread_mutex_init(&mu, &at);

    if (pthread_create(&t, NULL, die_holding, &mu) != 0 || pthread_join(t, NULL) != 0) return 27;
    if (pthread_mutex_lock(&mu) != EOWNERDEAD) return 28;

    pthread_mutex_consistent(&mu);
    pthread_mutex_unlock(&mu);

    /* the real-time timer: SIGALRM once after the value, again after the interval, and what is left is told back */
    struct sigaction sa = { .sa_handler = on_alarm };
    struct itimerval it = { .it_value = { 0, 50000 } }, old;

    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGALRM, &sa, NULL) != 0) return 60;

    alarms = 0;
    if (setitimer(ITIMER_REAL, &it, NULL) != 0) return 61;
    if (getitimer(ITIMER_REAL, &old) != 0 || old.it_value.tv_sec != 0 || old.it_value.tv_usec <= 0 || old.it_value.tv_usec > 50000) return 62;

    for (int i = 0; i < 100 && !alarms; i++) usleep(10000);

    if (alarms != 1) return 63;

    it = (struct itimerval){ .it_interval = { 0, 20000 }, .it_value = { 0, 20000 } };
    alarms = 0;

    if (setitimer(ITIMER_REAL, &it, NULL) != 0) return 64;

    for (int i = 0; i < 200 && alarms < 3; i++) usleep(10000);

    /* stopped by a zero value, which gives back what was running */
    it = (struct itimerval){ 0 };
    if (setitimer(ITIMER_REAL, &it, &old) != 0 || old.it_interval.tv_usec != 20000) return 65;
    if (alarms < 3) return 66;

    usleep(100000);
    alarms = 0;
    usleep(100000);
    if (alarms != 0) return 67;

    /* alarm(): seconds, and what was left of the one before */
    if (alarm(30) != 0) return 68;
    if (alarm(0) != 30) return 69;

    return 0x5e;
}
