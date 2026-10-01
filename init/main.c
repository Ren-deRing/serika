#include <serika/arch.h>
#include <serika/boot.h>
#include <serika/compiler.h>
#include <serika/cpu.h>
#include <serika/ftrace.h>
#include <serika/patch.h>
#include <serika/printk.h>
#include <serika/serial.h>
#include <serika/buddy.h>

notrace void start_init(void) {
    serial_init();
    set_log_putc(serial_putc);

    printk("\x1b[2J\x1b[H");

    // ftrace_enable();

    cpu_init();

    asm volatile ("int $3"); // for test

    printk("it looks like a clocksource will be needed.\n");
    printk("tsc freq is: %lu\n", curcpu->tsc_freq_hz);

    printk("LFB: %dx%d\n", g_bootinfo.fb.width, g_bootinfo.fb.height);
    printk("MAX_PHYS_ADDR: 0x%lx\n", g_bootinfo.mem.max_phys_addr);

    buddy_init();

    void* page = alloc_pages(4);
    printk("alloc: 0x%lx\n", (uintptr_t)page);

    void* buddy = alloc_pages(4);
    printk("buddy: 0x%lx\n", (uintptr_t)buddy);

    free_pages(page, 4);
    void* order5 = alloc_pages(5);
    printk("free page1, order5: 0x%lx\n", (uintptr_t)order5);
    
    free_pages(buddy, 4);
    void* parent = alloc_pages(5);
    printk("free buddy, parent: 0x%lx\n", (uintptr_t)parent);

    for (;;) arch_halt();
}
