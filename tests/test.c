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
    { "slab remote", test_slab_remote },
    { "slab smp",    test_slab_smp },
    { "slab reclaim", test_slab_reclaim },
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
    { "block read",  test_block_read },
    { "block write", test_block_write },
    { "ext4 read",   test_ext4_read },
    { "ext4 crc32c", test_ext4_crc32c },
    { "ext4 csum",   test_ext4_csum_kinds },
    { "ext4 csum dir", test_ext4_csum_dir },
    { "ext4 mount csum", test_ext4_mount_csum },
    { "ext4 read csum", test_ext4_read_csum },
    { "ext4 bflush", test_ext4_bflush },
    { "ext4 sync",   test_ext4_sync },
    { "ext4 inode",  test_ext4_inode },
    { "ext4 alloc",  test_ext4_alloc },
    { "ext4 extent", test_ext4_extent },
    { "ext4 write",  test_ext4_write },
    { "ext4 persist", test_ext4_persist },
    { "ext4 namei",  test_ext4_namei },
    { "ext4 namei index", test_ext4_namei_index },
    { "ext4 namei persist", test_ext4_namei_persist },
    { "vfs pivot",   test_vfs_pivot },
    { "ext4 umount", test_ext4_umount },
    { "vfs remove",  test_vfs_remove },
    { "net ping lo", test_net_ping_lo },
    { "net arp",     test_net_arp },
    { "net ping gw", test_net_ping_gw },
    { "net udp",     test_net_udp },
    { "exec",        test_exec },
    { "exec glibc",  test_exec_glibc },
    { "pipe",        test_pipe },
    { "tty ldisc",   test_tty_ldisc },
    { "exec pty",    test_exec_pty },
    { "hda play",    test_hda_play },
    { "exec files",  test_exec_files },
    { "exec busybox", test_exec_busybox },
    { "exec syscalls", test_exec_syscalls },
    { "exec gl",     test_exec_gl },
    { "exec sockets", test_exec_sockets },
    { "exec udp",    test_exec_udp },
    { "exec events", test_exec_events },
    { "exec drm",    test_exec_drm },
    { "exec gbm",    test_exec_gbm },
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
