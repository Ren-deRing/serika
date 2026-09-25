#include <serika/arch.h>
#include <serika/compiler.h>
#include <serika/cpu.h>
#include <serika/ftrace.h>
#include <serika/printk.h>
#include <serika/serial.h>

notrace void start_init(void) {
    serial_init();
    set_log_putc(serial_putc);

    printk("\x1b[2J\x1b[H");

    ftrace_enable();

    cpu_init();

    asm volatile ("int $3"); // for test

    for (;;) arch_halt();
}
