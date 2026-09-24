#include <serika/compiler.h>
#include <serika/printk.h>
#include <serika/serial.h>

#include <asm/ftrace.h>

notrace void start_init(void) {
    serial_init();
    set_log_putc(serial_putc);

    printk("\x1b[2J\x1b[H");

    init_ftrace();

    for (;;);
}
