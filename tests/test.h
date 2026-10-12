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

struct task;

/* tests/util.c. reaped id, 0 on timeout */
long reap_within(struct task *t, long *status, uint64_t ms);
long run_user(const uint8_t *code, size_t size, long *status);
long run_exec(const char *path, long *status); /* the program's argv and envp are fixed */
long run_exec_mode(const char *path, const char *mode, long *status);

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
bool test_user_stack();
bool test_thread_mm();
bool test_thread_glibc();
bool test_fork_glibc();

bool test_wait_sleep();
bool test_wait_pingpong();
bool test_wait_child();
bool test_wait_orphan();

bool test_mutex();

bool test_block_read();

bool test_vfs_basic();
bool test_vfs_initrd();
bool test_vfs_symlink();
bool test_vfs_remove();
bool test_ext4_read();
bool test_exec();
bool test_exec_glibc();
bool test_exec_files();
bool test_pipe();
bool test_tty_ldisc();
bool test_exec_busybox();
bool test_exec_syscalls();
bool test_exec_gl();
bool test_exec_sockets();
bool test_exec_events();

bool test_signal_fault();
bool test_signal_kill();
bool test_signal_glibc();
