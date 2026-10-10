#include <serika/arch.h>
#include <serika/cpu.h>
#include <serika/sched.h>

#include <asm/arch.h>

#include <tests/test.h>

static volatile bool     stop;
static volatile uint64_t count[2];
static volatile bool     done[2];

static void worker(int i) {
    while (!stop) count[i]++;
    done[i] = true;
}

static void worker0(void) { worker(0); }
static void worker1(void) { worker(1); }

/* run two spinning workers for ~300ms, return count[0] / count[1] in percent */
static uint64_t run_pair(int nice0, int nice1) {
    stop = false;
    count[0] = count[1] = 0;
    done[0]  = done[1]  = false;

    struct task *a = task_create(worker0, nice0);
    struct task *b = task_create(worker1, nice1);
    if (!a || !b) return 0;

    /* same cpu as us, or they never compete */
    a->pinned = b->pinned = true;
    sched_add_task_cpu(a, curcpu->id);
    sched_add_task_cpu(b, curcpu->id);

    /* we compete too. if preemption is broken, we never come back */
    uint64_t until = rdtsc() + curcpu->tsc_freq_hz / 1000 * 300;
    while (rdtsc() < until) arch_pause();

    stop = true;
    while (!done[0] || !done[1]) arch_pause();

    if (!count[1]) return 0;
    return count[0] * 100 / count[1];
}

bool test_sched_fair() {
    uint64_t ratio = run_pair(0, 0);

    TEST_ASSERT(ratio >= 90 && ratio <= 110); /* 1:1 */
    return true;
}

bool test_sched_nice() {
    uint64_t ratio = run_pair(0, 5);

    TEST_ASSERT(ratio >= 260 && ratio <= 350); /* 1024 / 335 = ~3.06 */
    return true;
}
