#include <serika/arch.h>
#include <serika/printk.h>

#include <stdarg.h>

void panic(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    printk("panic: ");
    vprintk(fmt, args);
    printk("\n");

    va_end(args);

    arch_panic();
}
