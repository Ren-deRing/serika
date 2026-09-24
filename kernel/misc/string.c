#include <stddef.h>
#include <stdint.h>

void *memset(void *dest, int ch, size_t count) {
    uint8_t *p = (uint8_t *)dest;
    for (size_t i = 0; i < count; i++) {
        p[i] = (uint8_t)ch;
    }
    return dest;
}

void *memcpy(void *dest, const void *src, size_t count) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < count; i++) {
        d[i] = s[i];
    }
    return dest;
}

void *memmove(void *dest, const void *src, size_t count) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        for (size_t i = 0; i < count; i++)
            d[i] = s[i];
    } else if (d > s) {
        for (size_t i = count; i > 0; i--)
            d[i - 1] = s[i - 1];
    }
    return dest;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *p1 = s1, *p2 = s2;

    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i])
            return p1[i] < p2[i] ? -1 : 1;
    }
    return 0;
}

size_t strlen(const char *s) {
    const char *p = s;
    while (*p) {
        p++;
    }
    return (size_t)(p - s);
}

size_t strnlen(const char *s, size_t maxlen) {
    size_t len;
    for (len = 0; len < maxlen && s[len] != '\0'; len++)
        ;
    return len;
}

char *strcpy(char *dest, const char *src) {
    char *p = dest;
    while ((*p++ = *src++)) { }
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
    size_t i;

    for (i = 0; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }

    for (; i < n; i++) {
        dest[i] = '\0';
    }

    return dest;
}

size_t strlcpy(char *dest, const char *src, size_t size) {
    size_t i;
    if (size == 0) return strlen(src);

    for (i = 0; i < size - 1 && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    dest[i] = '\0';

    return strlen(src);
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    if (n == 0) return 0;
    
    while (n-- > 0) {
        if (*s1 != *s2)
            return *(const unsigned char *)s1 - *(const unsigned char *)s2;
        if (*s1 == '\0')
            break;
        s1++;
        s2++;
    }
    return 0;
}

char *strrchr(const char *s, int c) {
    char *last = NULL;
    char ch = (char)c;

    do {
        if (*s == ch) {
            last = (char *)s;
        }
    } while (*s++);

    return last;
}