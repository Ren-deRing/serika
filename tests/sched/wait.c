#include <serika/arch.h>
#include <serika/cpu.h>
#include <serika/errno.h>
#include <serika/preempt.h>
#include <serika/sched.h>
#include <serika/syscall.h>
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

/* loops a while unless fast, then exits with the code */
static const uint8_t exit_code[] = {
    0xb9, 0x00, 0x00, 0x00, 0x04,       /* mov $0x4000000, %ecx  */
    0xff, 0xc9,                         /* 1: dec %ecx           */
    0x75, 0xfc,                         /* jnz 1b                */
    0xbf, 0x00, 0x00, 0x00, 0x00,       /* mov $code, %edi       */
    0xb8, 0x3c, 0x00, 0x00, 0x00,       /* mov $60, %eax (exit)  */
    0x0f, 0x05,                         /* syscall (SYS_EXIT)    */
    0xeb, 0xfe,                         /* jmp .                 */
};

static struct task *spawn_exit(uint8_t code, bool fast) {
    uint8_t c[sizeof(exit_code)];
    for (size_t i = 0; i < sizeof(c); i++) c[i] = exit_code[i];

    /* one round */
    if (fast) {
        c[1] = 0x01;
        c[4] = 0x00;
    }

    c[10] = code;

    struct task *t = task_create_user(c, sizeof(c), 0);
    if (t) sched_add_task(t);

    return t;
}

static volatile bool wc_done;
static volatile long wc_nohang, wc_mask, wc_echild;

/* sleeps in task_wait() until each child exits */
static void wait_child_main(void) {
    for (int i = 1; i <= 3; i++)
        if (!spawn_exit(i, false)) return;

    wc_nohang = task_wait(NULL, NULL, WNOHANG); /* all still looping */

    for (int i = 0; i < 3; i++) {
        long status = 0;
        if (task_wait(NULL, &status, 0) > 0) wc_mask |= 1 << WEXITSTATUS(status);
    }

    wc_echild = task_wait(NULL, NULL, 0);
    wc_done = true;
}

bool test_wait_child() {
    wc_done = false;
    wc_nohang = -1;
    wc_mask = 0;
    wc_echild = 0;

    struct task *t = task_create(wait_child_main, 0);
    TEST_ASSERT(t);
    sched_add_task(t);

    TEST_ASSERT(wait_for(&wc_done, 2000));
    TEST_ASSERT(wc_nohang == 0);
    TEST_ASSERT(wc_mask == 0xe);
    TEST_ASSERT(wc_echild == -ECHILD);
    return true;
}

static volatile bool orphan_done;
static volatile bool orphan_ok;

/* exits with one zombie child and one still running */
static void orphan_main(void) {
    struct task *zombie = spawn_exit(0, true);
    struct task *running = spawn_exit(0, false);

    orphan_ok = zombie && running;

    /* ours until we reap it or exit */
    while (zombie && __atomic_load_n(&zombie->state, 2) != TASK_ZOMBIE) arch_pause(); /* __ATOMIC_ACQUIRE */

    orphan_done = true;
}

bool test_wait_orphan() {
    orphan_done = false;
    orphan_ok = false;

    struct task *t = task_create(orphan_main, 0);
    TEST_ASSERT(t);
    sched_add_task(t);

    TEST_ASSERT(wait_for(&orphan_done, 1000));
    TEST_ASSERT(orphan_ok);

    /* the running one exits with no parent and frees itself */
    uint64_t until = rdtsc() + curcpu->tsc_freq_hz / 5; /* 200ms */
    while (rdtsc() < until) arch_pause();

    /* orphans are not ours */
    TEST_ASSERT(task_wait(NULL, NULL, WNOHANG) == -ECHILD);
    return true;
}
