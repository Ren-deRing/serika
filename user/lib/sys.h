#pragma once

#include <stddef.h>
#include <stdint.h>

#define AT_NULL   0
#define AT_PHDR   3
#define AT_PHENT  4
#define AT_PHNUM  5
#define AT_PAGESZ 6
#define AT_ENTRY  9
#define AT_RANDOM 25
#define AT_EXECFN 31

static inline int streq(const char *a, const char *b) {
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}

void *memset(void *dest, int c, size_t n);
void *memcpy(void *dest, const void *src, size_t n);
