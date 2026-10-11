#pragma once

#include <serika/arch.h>
#include <serika/cpu.h>
#include <serika/printk.h>
#include <asm/arch.h>
#include <stdbool.h>
#include <stdint.h>

#define TEST_ASSERT(cond)                      \
    do {                                       \
        if (!(cond)) {                         \
            printk("[FAIL] %s:%d: %s\n",       \
                   __FILE__, __LINE__, #cond); \
            return false;                      \
        }                                      \
    } while (0)

static uint32_t rand_state = 0x12345678;
static inline uint32_t rand_u32(void) {
    uint32_t x = rand_state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;

    rand_state = x;
    return x;
}

/* poll, not sleep: a broken wait must not hang the test runner */
static inline bool wait_for(volatile bool *flag, uint64_t ms) {
    uint64_t until = rdtsc() + curcpu->tsc_freq_hz / 1000 * ms;

    while (!*flag && rdtsc() < until) arch_pause();

    return *flag;
}

void test_run_all();

bool test_buddy_merge();
bool test_buddy_partial_buddy();

bool test_slab_reuse();
bool test_slab_multi();
bool test_slab_stress();
bool test_slab_state();

bool test_mm_fault();
bool test_tlb_storm();
bool test_tlb_mprotect();
bool test_tlb_munmap();

bool test_tick_rate();

bool test_sched_fair();
bool test_sched_nice();

bool test_smp_online();
bool test_smp_spread();
bool test_smp_balance();

bool test_user_task();
bool test_user_fault();
bool test_user_thread();
bool test_user_kill();

bool test_wait_sleep();
bool test_wait_pingpong();
bool test_wait_child();
bool test_wait_orphan();

bool test_mutex();

bool test_vfs_basic();
bool test_vfs_initrd();
bool test_exec();
bool test_exec_glibc();
bool test_exec_files();
bool test_exec_signals();
