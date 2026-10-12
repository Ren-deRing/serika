#include <serika/arch.h>
#include <serika/boot.h>
#include <serika/compiler.h>
#include <serika/cpu.h>
#include <serika/exec.h>
#include <serika/ftrace.h>
#include <serika/irq.h>
#include <serika/patch.h>
#include <serika/panic.h>
#include <serika/pmap.h>
#include <serika/printk.h>
#include <serika/random.h>
#include <serika/serial.h>
#include <serika/buddy.h>
#include <serika/slab.h>
#include <serika/sched.h>
#include <serika/symbol.h>
#include <serika/syscall.h>
#include <serika/tick.h>
#include <serika/vfs.h>

#include <asm/acpi.h>

#include <tests/test.h>

/* busybox's sh on the console, and back here once it is done */
static void run_shell(void) {
    const char *const argv[] = { "sh", NULL };
    const char *const envp[] = { "PATH=/bin", "HOME=/", "TERM=linux", NULL };
    struct task *shell;

    int err = task_create_exec("/bin/sh", argv, envp, 0, &shell);
    if (err) {
        printk("init: unable to start /bin/sh -- error %d\n", err);
        return;
    }

    sched_add_task(shell);

    long status = 0;
    task_wait(shell, &status, 0);

    printk("init: the shell exited with %ld\n", WEXITSTATUS(status));
}

notrace void start_init(void) {
    serial_init();
    set_log_putc(serial_putc);

    printk("\x1b[2J\x1b[H");

    // ftrace_enable();

    printk("Hello serika\n");

    cpu_init();
    random_init();
    buddy_init();
    slab_init();
    pmap_init();
    kmap_init();

    sched_init();
    tick_init();

    extern void smp_release_aps(void);
    smp_release_aps();

    printk("smp: %u cpus online\n", g_bootinfo.smp.total_cores);

    acpi_init();
    ioapic_init();
    serial_tty_init();

    vfs_init();
    initramfs_unpack();
    
    test_run_all();

    run_shell();

    for (;;) arch_halt();
}
