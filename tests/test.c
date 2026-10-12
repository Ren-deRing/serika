#include <serika/arch.h>
#include <serika/preempt.h>
#include <serika/printk.h>
#include <tests/test.h>

#include <stddef.h>

typedef bool (*test_fn_t)(void);

struct test {
    const char *name;
    test_fn_t fn;
};

static struct test tests[] = {
    { "buddy merge", test_buddy_merge },
    { "buddy partial", test_buddy_partial_buddy },
    { "slab reuse",  test_slab_reuse },
    { "slab multi",  test_slab_multi },
    { "slab stress", test_slab_stress },
    { "slab state",  test_slab_state },
    { "mm fault",    test_mm_fault },
    { "tick rate",   test_tick_rate },
    { "sched fair",  test_sched_fair },
    { "sched nice",  test_sched_nice },
    { "smp online",  test_smp_online },
    { "smp spread",  test_smp_spread },
    { "smp balance", test_smp_balance },
    { "user task",   test_user_task },
    { "user stack",  test_user_stack },
    { "signal fault", test_signal_fault },
    { "thread mm",   test_thread_mm },
    { "signal kill", test_signal_kill },
    { "tlb storm",   test_tlb_storm },
    { "tlb mprotect", test_tlb_mprotect },
    { "tlb munmap",  test_tlb_munmap },
    { "wait sleep",  test_wait_sleep },
    { "wait pingpong", test_wait_pingpong },
    { "wait child",  test_wait_child },
    { "wait orphan", test_wait_orphan },
    { "mutex",       test_mutex },
    { "vfs basic",   test_vfs_basic },
    { "vfs initrd",  test_vfs_initrd },
    { "vfs symlink", test_vfs_symlink },
    { "vfs remove",  test_vfs_remove },
    { "exec",        test_exec },
    { "exec glibc",  test_exec_glibc },
    { "pipe",        test_pipe },
    { "tty ldisc",   test_tty_ldisc },
    { "exec files",  test_exec_files },
    { "exec busybox", test_exec_busybox },
    { "exec syscalls", test_exec_syscalls },
    { "signal glibc", test_signal_glibc },
    { "fork glibc",  test_fork_glibc },
    { "thread glibc", test_thread_glibc },
};

void test_run_all() {
    size_t passed = 0;
    size_t total = sizeof(tests) / sizeof(tests[0]);

    printk("[TEST] running %zu tests\n", total);

    for (size_t i = 0; i < total; i++) {
        uint32_t count = preempt_count();
        bool irq_off = arch_irqs_disabled();

        bool result = tests[i].fn();

        /* a test that leaves a lock held, or irq off, breaks everything after it */
        if (preempt_count() != count || arch_irqs_disabled() != irq_off) {
            printk("[FAIL] %s left preempt_count 0x%x -> 0x%x, irq %s -> %s\n", tests[i].name,
                   count, preempt_count(), irq_off ? "off" : "on", arch_irqs_disabled() ? "off" : "on");
            result = false;
        }

        printk("[TEST] %-24s %s\n", tests[i].name, result ? "PASS" : "FAIL");

        if (result) passed++;
    }

    printk("[TEST] %zu/%zu passed\n", passed, total);
}
