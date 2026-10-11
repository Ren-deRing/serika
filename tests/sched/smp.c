#include <serika/arch.h>
#include <serika/boot.h>
#include <serika/cpu.h>
#include <serika/sched.h>

#include <asm/arch.h>

#include <tests/test.h>

static volatile bool     stop;
static volatile uint8_t  ran[MAX_CPUS]; /* spinner ran on this cpu */
static volatile uint32_t finished;

static void spinner(void) {
    while (!stop) ran[curcpu->id] = 1;
    __atomic_fetch_add(&finished, 1, 5);
}

static uint32_t cpus_ran(void) {
    uint32_t n = 0;

    for (uint32_t i = 0; i < MAX_CPUS; i++) n += ran[i];

    return n;
}

bool test_smp_online() {
    uint32_t ncpus = g_bootinfo.smp.total_cores ? g_bootinfo.smp.total_cores : 1;

    TEST_ASSERT(sched_nr_online() == ncpus);
    return true;
}

bool test_smp_spread() {
    uint32_t ncpus = sched_nr_online();
    uint32_t n = ncpus * 2;

    stop = false;
    for (uint32_t i = 0; i < MAX_CPUS; i++) ran[i] = 0;
    finished = 0;

    for (uint32_t i = 0; i < n; i++) {
        struct task *t = task_create(spinner, 0);
        TEST_ASSERT(t);
        sched_add_task(t);
    }

    uint64_t until = rdtsc() + curcpu->tsc_freq_hz / 1000 * 100; /* 100ms */
    while (rdtsc() < until) arch_pause();

    stop = true;
    while (finished < n) arch_pause();

    TEST_ASSERT(cpus_ran() == ncpus); /* every cpu ran something */
    return true;
}

/* everything starts on our cpu. balancing has to spread it out */
bool test_smp_balance() {
    uint32_t ncpus = sched_nr_online();
    uint32_t n = ncpus * 2;

    stop = false;
    for (uint32_t i = 0; i < MAX_CPUS; i++) ran[i] = 0;
    finished = 0;

    for (uint32_t i = 0; i < n; i++) {
        struct task *t = task_create(spinner, 0);
        TEST_ASSERT(t);
        sched_add_task_cpu(t, curcpu->id);
    }

    uint64_t until = rdtsc() + curcpu->tsc_freq_hz / 1000 * 100; /* 100ms */
    while (rdtsc() < until) arch_pause();

    uint32_t min = ~0u, max = 0;
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        if (!sched_cpu_online(i)) continue;

        uint32_t nr = sched_nr_running(i);
        if (nr < min) min = nr;
        if (nr > max) max = nr;
    }

    stop = true;
    while (finished < n) arch_pause();

    TEST_ASSERT(cpus_ran() == ncpus);
    TEST_ASSERT(max - min <= 1);
    return true;
}
