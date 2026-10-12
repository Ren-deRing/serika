#include <serika/arch.h>
#include <serika/buddy.h>
#include <serika/cpu.h>
#include <serika/page.h>
#include <serika/preempt.h>
#include <serika/sched.h>
#include <serika/slab.h>
#include <serika/string.h>
#include <stdint.h>
#include <tests/test.h>

#include <asm/arch.h>

#define STRESS_SLOTS 256
#define STRESS_ITERS 100000

#define STATE_TEST_SIZE 15
#define STATE_TEST_OBJECTS (PAGE_SIZE / 16)

bool test_slab_reuse() {
    /* the freelist is a stack of the cpu's: the same cpu for both, or another has its own */
    preempt_count_add(1);

    void *a = kmalloc(24);
    TEST_ASSERT(a != NULL);

    kfree(a);

    void *b = kmalloc(17);
    TEST_ASSERT(b == a);

    kfree(b);

    preempt_count_sub(1);
    return true;
}

bool test_slab_multi() {
    void *a = kmalloc(24);
    void *b = kmalloc(24);
    void *c = kmalloc(24);

    TEST_ASSERT(a != NULL);
    TEST_ASSERT(b != NULL);
    TEST_ASSERT(c != NULL);

    TEST_ASSERT(a != b);
    TEST_ASSERT(a != c);
    TEST_ASSERT(b != c);

    kfree(a);
    kfree(b);
    kfree(c);

    return true;
}

bool test_slab_stress() {
    void *slots[STRESS_SLOTS] = { 0 };
    size_t sizes[STRESS_SLOTS] = { 0 };

    for (size_t i = 0; i < STRESS_ITERS; i++) {
        size_t idx = rand_u32() % STRESS_SLOTS;

        if (slots[idx]) {
            uint8_t *ptr = slots[idx];

            for (size_t j = 0; j < sizes[idx]; j++)
                TEST_ASSERT(ptr[j] == 0xA5);

            kfree(slots[idx]);
            slots[idx] = NULL;
        } else {
            size_t size = (rand_u32() % 4096) + 1;
            void *ptr = kmalloc(size);

            TEST_ASSERT(ptr != NULL);

            memset(ptr, 0xA5, size);

            slots[idx] = ptr;
            sizes[idx] = size;
        }
    }

    for (size_t i = 0; i < STRESS_SLOTS; i++) {
        if (slots[i])
            kfree(slots[i]);
    }

    return true;
}

bool test_slab_state() {
    void *objects[STATE_TEST_OBJECTS];

    /* empty -> partial */
    objects[0] = kmalloc(STATE_TEST_SIZE);
    TEST_ASSERT(objects[0] != NULL);

    /* partial -> full */
    for (size_t i = 1; i < STATE_TEST_OBJECTS; i++) {
        objects[i] = kmalloc(STATE_TEST_SIZE);
        TEST_ASSERT(objects[i] != NULL);
    }

    /* full -> partial */
    kfree(objects[0]);

    /* partial -> empty */
    for (size_t i = 1; i < STATE_TEST_OBJECTS; i++)
        kfree(objects[i]);

    /* empty -> partial again */
    void *reuse = kmalloc(STATE_TEST_SIZE);
    TEST_ASSERT(reuse != NULL);

    kfree(reuse);

    return true;
}

/* ---- more than one cpu ---- */

#define REMOTE_OBJECTS 200
#define REMOTE_SIZE    128

static void *remote_objs[REMOTE_OBJECTS];
static volatile bool remote_done;

static void remote_freer(void) {
    for (int i = 0; i < REMOTE_OBJECTS; i++) kfree(remote_objs[i]);

    __atomic_store_n(&remote_done, true, __ATOMIC_RELEASE);
}

/* a cpu other than this one, or this one if it is alone */
static uint32_t other_cpu(void) {
    uint32_t me = curcpu->id;

    for (uint32_t i = 0; i < MAX_CPUS; i++)
        if (i != me && sched_cpu_online(i)) return i;

    return me;
}

/* a task for cpu, which stays there */
static bool run_on(uint32_t cpu, void (*fn)(void)) {
    struct task *t = task_create(fn, 0);
    if (!t) return false;

    t->pinned = true;
    sched_add_task_cpu(t, cpu);
    return true;
}

/* objects freed by another cpu come back, once and whole */
bool test_slab_remote() {
    for (int i = 0; i < REMOTE_OBJECTS; i++) {
        remote_objs[i] = kmalloc(REMOTE_SIZE);
        TEST_ASSERT(remote_objs[i] != NULL);
        memset(remote_objs[i], 0x5A, REMOTE_SIZE);
    }

    remote_done = false;
    TEST_ASSERT(run_on(other_cpu(), remote_freer));

    while (!__atomic_load_n(&remote_done, __ATOMIC_ACQUIRE)) arch_pause();

    /* and used again: no object twice */
    void *again[REMOTE_OBJECTS];

    for (int i = 0; i < REMOTE_OBJECTS; i++) {
        again[i] = kmalloc(REMOTE_SIZE);
        TEST_ASSERT(again[i] != NULL);

        for (int j = 0; j < i; j++) TEST_ASSERT(again[j] != again[i]);

        memset(again[i], i, REMOTE_SIZE);
    }

    for (int i = 0; i < REMOTE_OBJECTS; i++) {
        for (int j = 0; j < REMOTE_SIZE; j++) TEST_ASSERT(((uint8_t *)again[i])[j] == (uint8_t)i);

        kfree(again[i]);
    }

    return true;
}

#define SMP_SLOTS 512
#define SMP_ITERS 150000
#define SMP_MAGIC 0x9e3779b97f4a7c15ULL

static void *volatile smp_slots[SMP_SLOTS];
static volatile uint32_t smp_finished;
static volatile uint32_t smp_bad;

static const size_t smp_sizes[] = { 16, 24, 40, 64, 100, 128, 200, 256, 500, 1000, 2000, 4000 };

/* what an object has in it: where it is, how big, then a byte for the rest */
static void smp_fill(void *p, size_t size) {
    uint64_t *h = p;

    h[0] = (uint64_t)p ^ SMP_MAGIC;
    h[1] = size;
    memset((uint8_t *)p + 16, (uint8_t)((uintptr_t)p >> 4), size - 16);
}

static bool smp_check(void *p) {
    uint64_t *h = p;

    if (h[0] != ((uint64_t)p ^ SMP_MAGIC) || h[1] < 16 || h[1] > 4096) return false;

    for (size_t i = 16; i < h[1]; i++)
        if (((uint8_t *)p)[i] != (uint8_t)((uintptr_t)p >> 4)) return false;

    return true;
}

static void smp_worker(void) {
    uint64_t x = 0x2545F4914F6CDD1DULL * (curcpu->id + 1); /* xorshift, its own for each */

    for (int i = 0; i < SMP_ITERS; i++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;

        size_t idx = x % SMP_SLOTS;
        void *old = __atomic_exchange_n(&smp_slots[idx], NULL, __ATOMIC_ACQ_REL);

        if (old) {
            if (!smp_check(old)) __atomic_add_fetch(&smp_bad, 1, __ATOMIC_RELAXED);

            kfree(old); /* made by this cpu or another */
            continue;
        }

        size_t size = smp_sizes[(x >> 20) % (sizeof(smp_sizes) / sizeof(smp_sizes[0]))];
        void *p = kmalloc(size);

        if (!p) {
            __atomic_add_fetch(&smp_bad, 1, __ATOMIC_RELAXED);
            continue;
        }

        smp_fill(p, size);

        void *expected = NULL;

        if (!__atomic_compare_exchange_n(&smp_slots[idx], &expected, p, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            if (!smp_check(p)) __atomic_add_fetch(&smp_bad, 1, __ATOMIC_RELAXED);

            kfree(p);
        }
    }

    __atomic_add_fetch(&smp_finished, 1, __ATOMIC_ACQ_REL);
}

/* every cpu allocates and frees at once, and what one made another frees */
bool test_slab_smp() {
    uint32_t n = 0;

    smp_bad = smp_finished = 0;

    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        if (!sched_cpu_online(i)) continue;

        TEST_ASSERT(run_on(i, smp_worker));
        n++;
    }

    while (__atomic_load_n(&smp_finished, __ATOMIC_ACQUIRE) < n) arch_pause();

    TEST_ASSERT(smp_bad == 0);

    for (int i = 0; i < SMP_SLOTS; i++) {
        void *p = smp_slots[i];

        if (!p) continue;

        TEST_ASSERT(smp_check(p));
        kfree(p);
        smp_slots[i] = NULL;
    }

    return true;
}

#define RECLAIM_OBJECTS 4000
#define RECLAIM_SIZE    1024
#define RECLAIM_SLACK   64 /* what the cpus and the cache keep: a few slabs of each */

/* the pages of slabs that are all free go back to the buddy */
bool test_slab_reclaim() {
    void **objs = alloc_pages(SIZE_TO_ORDER(RECLAIM_OBJECTS * sizeof(void *)));
    TEST_ASSERT(objs);

    objs = (void **)P2V((uintptr_t)objs);

    size_t before = buddy_free_pages();

    for (int i = 0; i < RECLAIM_OBJECTS; i++) {
        objs[i] = kmalloc(RECLAIM_SIZE);
        TEST_ASSERT(objs[i] != NULL);
    }

    /* real pages went to the slabs: not all of the objects, since slabs of the tests before have free room */
    long held = (long)before - (long)buddy_free_pages();
    TEST_ASSERT(held > 2 * RECLAIM_SLACK);

    for (int i = 0; i < RECLAIM_OBJECTS; i++) kfree(objs[i]);

    /* signed: slabs that were free before may go as well */
    long kept = (long)before - (long)buddy_free_pages();
    TEST_ASSERT(kept <= RECLAIM_SLACK);

    free_pages((void *)V2P((uintptr_t)objs), SIZE_TO_ORDER(RECLAIM_OBJECTS * sizeof(void *)));
    return true;
}
