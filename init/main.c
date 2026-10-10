#include <serika/arch.h>
#include <serika/boot.h>
#include <serika/compiler.h>
#include <serika/cpu.h>
#include <serika/ftrace.h>
#include <serika/patch.h>
#include <serika/panic.h>
#include <serika/pmap.h>
#include <serika/printk.h>
#include <serika/serial.h>
#include <serika/buddy.h>
#include <serika/slab.h>
#include <serika/sched.h>
#include <serika/symbol.h>
#include <serika/tick.h>

#include <tests/test.h>

notrace void start_init(void) {
    serial_init();
    set_log_putc(serial_putc);

    printk("\x1b[2J\x1b[H");

    // ftrace_enable();

    printk("Hello serika\n");

    cpu_init();
    buddy_init();
    slab_init();
    pmap_init();
    kmap_init();

    sched_init();
    tick_init();

    extern void smp_release_aps(void);
    smp_release_aps();

    printk("smp: %u cpus online\n", g_bootinfo.smp.total_cores);
    
    test_run_all();

    for (;;) arch_halt();
}
