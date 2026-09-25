#include <serika/compiler.h>
#include <serika/printk.h>

#include <stdarg.h>

#define STB_SPRINTF_IMPLEMENTATION
#include <deps/stb_sprintf.h>

static log_putc printk_putc;

void set_log_putc(log_putc new_putc) {
    printk_putc = new_putc;
}

notrace static void putc(char c) {
    if (printk_putc) {
        printk_putc(c);
    }
}

notrace static char* _stb_callback(char const *buf, void *user, int len) {
    (void)user;

    for (int i = 0; i < len; i++) {
        putc(buf[i]);
    }
    return (char *)buf;
}

notrace void vprintk(const char *fmt, va_list args) {
    char buf[STB_SPRINTF_MIN];

    stbsp_vsprintfcb(_stb_callback, NULL, buf, fmt, args);
}

notrace void printk(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);

    vprintk(fmt, args);

    va_end(args);
}
