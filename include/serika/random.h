#pragma once

#include <stddef.h>
#include <stdint.h>

void random_init(void);                      /* after cpu_init() */
void get_random_bytes(void *buf, size_t n);  /* any context with irq on or off */
