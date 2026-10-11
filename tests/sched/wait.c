#include <serika/arch.h>
#include <serika/cpu.h>
#include <serika/preempt.h>
#include <serika/sched.h>
#include <serika/wait.h>

#include <asm/arch.h>

#include <tests/test.h>

static struct wait_queue sleep_wq;
static volatile bool     sleep_go;
static volatile bool     sleep_woke;

static void sleeper(void) {
    wait_event(sleep_wq, sleep_go);
    sleep_woke = true;
}

bool test_wait_sleep() {
    wait_queue_init(&sleep_wq);
    sleep_go = false;
    sleep_woke = false;

    struct task *t = task_create(sleeper, 0);
    TEST_ASSERT(t);
    sched_add_task(t);

    /* it has to get off its rq, not spin in the loop */
    uint64_t until = rdtsc() + curcpu->tsc_freq_hz / 10; /* 100ms */
    while (!sched_task_sleeping(t) && rdtsc() < until) arch_pause();
    TEST_ASSERT(sched_task_sleeping(t));

    /* nobody woke it: it stays asleep */
    until = rdtsc() + curcpu->tsc_freq_hz / 50; /* 20ms */
    while (rdtsc() < until) arch_pause();
    TEST_ASSERT(sched_task_sleeping(t));
    TEST_ASSERT(!sleep_woke);

    /* t exits right after this, don't touch it again */
    sleep_go = true;
    wake_up(&sleep_wq);

    TEST_ASSERT(wait_for(&sleep_woke, 100));
    return true;
}

/* two threads pass a turn through two queues. a lost wakeup or a deadlock stops it */
#define PINGPONG_ROUNDS 2000

static struct wait_queue pp_wq[2];
static volatile uint32_t pp_turn;
static volatile uint32_t pp_finished;
static volatile bool     pp_done;

static void pingpong(uint32_t me) {
    uint32_t other = !me;

    for (int i = 0; i < PINGPONG_ROUNDS; i++) {
        wait_event(pp_wq[me], pp_turn == me);

        pp_turn = other;
        wake_up(&pp_wq[other]);
    }

    if (__atomic_add_fetch(&pp_finished, 1, 5) == 2) pp_done = true;
}

static void ping(void) { pingpong(0); }
static void pong(void) { pingpong(1); }

bool test_wait_pingpong() {
    wait_queue_init(&pp_wq[0]);
    wait_queue_init(&pp_wq[1]);
    pp_turn = 0;
    pp_finished = 0;
    pp_done = false;

    struct task *a = task_create(ping, 0);
    struct task *b = task_create(pong, 0);
    TEST_ASSERT(a && b);

    sched_add_task(a);
    sched_add_task(b);

    TEST_ASSERT(wait_for(&pp_done, 2000));
    return true;
}
