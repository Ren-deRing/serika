#pragma once

#include <serika/compiler.h>

typedef void (*log_putc)(char c);

notrace void printk(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

void set_log_putc(log_putc new_putc);
