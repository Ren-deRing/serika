#pragma once

#include <serika/compiler.h>

#include <stdarg.h>

typedef void (*log_putc)(char c);

notrace void printk(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
notrace void vprintk(const char *fmt, va_list args);

void set_log_putc(log_putc new_putc);
