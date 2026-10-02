#include <serika/arch.h>
#include <serika/boot.h>
#include <serika/compiler.h>
#include <serika/cpu.h>
#include <serika/ftrace.h>
#include <serika/patch.h>
#include <serika/printk.h>
#include <serika/serial.h>
#include <serika/buddy.h>
#include <serika/slab.h>

#include <serika/symbol.h>
#include <stdint.h>
#include <tests/test.h>

notrace void start_init(void) {
    serial_init();
    set_log_putc(serial_putc);

    printk("\x1b[2J\x1b[H");

    ftrace_enable();

    cpu_init();
    buddy_init();
    slab_init();

    const struct ksymbol *symbol = ksymbol_find((uintptr_t)0xffffffff820041e0);
    printk("symbol information: addr: %lx, name offset: %d, size: %d\n", symbol->addr, symbol->name_offset, symbol->size);
    printk("symbol name: %s\n", ksymbol_name(symbol));

    test_run_all();

    for (;;) arch_halt();
}
