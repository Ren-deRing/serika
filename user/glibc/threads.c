#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* exits 0x5e if every check holds, else the number of the failed one.
   argv[1] picks how the process ends instead, for the kernel tests: exitgroup or leaderexit */

#define THREADS 4
#define ROUNDS  20000
#define ITEMS   1000
#define CHURN   50

static long gettid_(void) {
    return syscall(SYS_gettid);
}

static long ms_since(const struct timespec *t0) {
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (t1.tv_sec - t0->tv_sec) * 1000 + (t1.tv_nsec - t0->tv_nsec) / 1000000;
}

/* contention on one mutex, from every cpu */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static long counter;
static long tids[THREADS], pids[THREADS];

static void *add(void *arg) {
    long i = (long)arg;

    tids[i] = gettid_();
    pids[i] = getpid();

    for (int r = 0; r < ROUNDS; r++) {
        pthread_mutex_lock(&lock);
        counter++;
        pthread_mutex_unlock(&lock);
    }

    return (void *)(i + 100);
}

/* each thread its own copy */
static __thread int tls_value = 7;

static void *tls_check(void *arg) {
    if (tls_value != 7) return (void *)1;

    tls_value = (int)(long)arg;
    usleep(5000);

    return (void *)(long)(tls_value == (int)(long)arg ? 0 : 2);
}

/* a queue of one slot */
static pthread_mutex_t qlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  qcond = PTHREAD_COND_INITIALIZER;
static int slot, full;
static long consumed;

static void *consumer(void *arg) {
    (void)arg;

    for (int i = 0; i < ITEMS; i++) {
        pthread_mutex_lock(&qlock);
        while (!full) pthread_cond_wait(&qcond, &qlock);

        consumed += slot;
        full = 0;

        pthread_cond_signal(&qcond);
        pthread_mutex_unlock(&qlock);
    }

    return NULL;
}

static volatile int detached_ran;

static void *detached(void *arg) {
    (void)arg;
    detached_ran = 1;
    return NULL;
}

static volatile long usr1_tid;
static volatile int  go;

static void on_usr1(int sig) {
    (void)sig;
    usr1_tid = gettid_();
}

static void *wait_for_usr1(void *arg) {
    (void)arg;

    while (!go) usleep(1000);
    return (void *)gettid_();
}

static void *nothing(void *arg) {
    return arg;
}

/* a process that forks from a thread: the child is the process's */
static void *forker(void *arg) {
    (void)arg;

    pid_t pid = fork();
    if (pid == 0) _exit(3);

    return (void *)(long)pid;
}

/* never woken: only the end of the process ends it */
static pthread_cond_t never = PTHREAD_COND_INITIALIZER;

static void *block_forever(void *arg) {
    (void)arg;

    pthread_mutex_lock(&qlock);
    for (;;) pthread_cond_wait(&never, &qlock);
}

static void *exit_soon(void *arg) {
    usleep(10000);
    exit((int)(long)arg);
}

int main(int argc, char **argv) {
    pthread_t t[THREADS];
    void *ret;

    /* exit() from one thread ends the others, blocked or not */
    if (argc > 1 && strcmp(argv[1], "exitgroup") == 0) {
        pthread_create(&t[0], NULL, block_forever, NULL);
        pthread_create(&t[1], NULL, exit_soon, (void *)0x42);
        pthread_join(t[0], NULL);
        return 1;
    }

    /* the main thread leaves first: the process lives on until the last thread */
    if (argc > 1 && strcmp(argv[1], "leaderexit") == 0) {
        pthread_create(&t[0], NULL, exit_soon, (void *)0x5e);
        pthread_exit(NULL);
    }

    /* 1. a mutex from every cpu, join's return values, ids */
    for (long i = 0; i < THREADS; i++)
        if (pthread_create(&t[i], NULL, add, (void *)i) != 0) return 1;

    for (long i = 0; i < THREADS; i++)
        if (pthread_join(t[i], &ret) != 0 || ret != (void *)(i + 100)) return 2;

    if (counter != (long)THREADS * ROUNDS) return 3;

    for (int i = 0; i < THREADS; i++) {
        if (pids[i] != getpid() || tids[i] == gettid_()) return 4;
        for (int j = 0; j < i; j++)
            if (tids[i] == tids[j]) return 4;
    }

    /* 2. thread-local storage */
    for (long i = 0; i < 3; i++)
        if (pthread_create(&t[i], NULL, tls_check, (void *)(i + 1)) != 0) return 5;
    for (int i = 0; i < 3; i++)
        if (pthread_join(t[i], &ret) != 0 || ret != NULL) return 6;
    if (tls_value != 7) return 7;

    /* 3. a condition variable both ways */
    if (pthread_create(&t[0], NULL, consumer, NULL) != 0) return 8;

    long produced = 0;
    for (int i = 1; i <= ITEMS; i++) {
        pthread_mutex_lock(&qlock);
        while (full) pthread_cond_wait(&qcond, &qlock);

        slot = i;
        full = 1;
        produced += i;

        pthread_cond_signal(&qcond);
        pthread_mutex_unlock(&qlock);
    }

    if (pthread_join(t[0], NULL) != 0 || consumed != produced) return 9;

    /* 4. a timed wait that times out */
    struct timespec t0, until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_nsec += 20 * 1000000;
    if (until.tv_nsec >= 1000000000) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000;
    }

    clock_gettime(CLOCK_MONOTONIC, &t0);
    pthread_mutex_lock(&qlock);
    int err = pthread_cond_timedwait(&never, &qlock, &until);
    pthread_mutex_unlock(&qlock);
    if (err != ETIMEDOUT || ms_since(&t0) < 15) return 10;

    /* 5. a detached thread, nobody joins it */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t[0], &attr, detached, NULL) != 0) return 11;
    for (int i = 0; i < 1000 && !detached_ran; i++) usleep(1000);
    if (!detached_ran) return 12;

    /* 6. a signal to the process, and one to a thread */
    signal(SIGUSR1, on_usr1);

    if (kill(getpid(), SIGUSR1) != 0 || usr1_tid == 0) return 13;

    usr1_tid = 0;
    if (pthread_create(&t[0], NULL, wait_for_usr1, NULL) != 0) return 14;
    if (pthread_kill(t[0], SIGUSR1) != 0) return 15;

    go = 1;
    if (pthread_join(t[0], &ret) != 0 || usr1_tid != (long)ret) return 16;

    /* 7. threads come and go */
    for (long i = 0; i < CHURN; i++)
        if (pthread_create(&t[0], NULL, nothing, (void *)i) != 0 || pthread_join(t[0], &ret) != 0 || ret != (void *)i)
            return 17;

    /* 8. a fork from a thread: main waits for it */
    if (pthread_create(&t[0], NULL, forker, NULL) != 0 || pthread_join(t[0], &ret) != 0) return 18;

    int status;
    if (waitpid((pid_t)(long)ret, &status, 0) != (pid_t)(long)ret || !WIFEXITED(status) || WEXITSTATUS(status) != 3)
        return 19;

    printf("threads: all checks passed\n");
    return 0x5e;
}
