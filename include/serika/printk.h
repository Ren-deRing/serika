#pragma once

#include <serika/compiler.h>

#include <stdarg.h>
#include <stddef.h>

typedef void (*log_putc)(char c);

notrace void printk(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
notrace void vprintk(const char *fmt, va_list args);

void set_log_putc(log_putc new_putc);

int ksnprintf(char *buf, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4))); /* the length it would have, as snprintf */
