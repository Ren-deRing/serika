#include <sys.h>

/* the compiler may call these on its own */
void *memset(void *dest, int c, size_t n) {
    unsigned char *d = dest;
    while (n--) *d++ = (unsigned char)c;
    return dest;
}

void *memcpy(void *dest, const void *src, size_t n) {
    unsigned char *d = dest;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dest;
}
