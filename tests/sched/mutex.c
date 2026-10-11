#include <serika/arch.h>
#include <serika/cpu.h>
#include <serika/mutex.h>
#include <serika/preempt.h>
#include <serika/sched.h>

#include <asm/arch.h>

#include <tests/test.h>

/* holders yield inside: without exclusion the counter loses updates */
#define MUTEX_THREADS 4
#define MUTEX_ROUNDS  500

static struct mutex      mx;
static volatile uint64_t mx_counter;
static volatile uint32_t mx_inside;
static volatile bool     mx_overlap;
static volatile uint32_t mx_finished;
static volatile bool     mx_done;

static void mutex_worker(void) {
    for (int i = 0; i < MUTEX_ROUNDS; i++) {
        mutex_lock(&mx);

        if (__atomic_add_fetch(&mx_inside, 1, 5) != 1) mx_overlap = true;

        uint64_t v = mx_counter;
        if (i % 8 == 0) schedule(); /* others pile up on the mutex */
        mx_counter = v + 1;

        __atomic_sub_fetch(&mx_inside, 1, 5);

        mutex_unlock(&mx);
    }

    if (__atomic_add_fetch(&mx_finished, 1, 5) == MUTEX_THREADS) mx_done = true;
}

bool test_mutex() {
    mutex_init(&mx);
    mx_counter = 0;
    mx_inside = 0;
    mx_overlap = false;
    mx_finished = 0;
    mx_done = false;

    for (int i = 0; i < MUTEX_THREADS; i++) {
        struct task *t = task_create(mutex_worker, 0);
        TEST_ASSERT(t);
        sched_add_task(t);
    }

    TEST_ASSERT(wait_for(&mx_done, 2000));
    TEST_ASSERT(!mx_overlap);
    TEST_ASSERT(mx_counter == MUTEX_THREADS * MUTEX_ROUNDS);

    /* uncontended, from the runner itself */
    TEST_ASSERT(mutex_trylock(&mx));
    TEST_ASSERT(!mutex_trylock(&mx));
    mutex_unlock(&mx);
    return true;
}
