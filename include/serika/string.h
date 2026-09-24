#pragma once

#include <stddef.h>

void *memset(void *dest, int ch, size_t count);
void *memcpy(void *dest, const void *src, size_t count);
int memcmp(const void *s1, const void *s2, size_t n);
void *memmove(void *dest, const void *src, size_t count);

size_t strlen(const char *s);
size_t strnlen(const char *s, size_t maxlen);

char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t n);
size_t strlcpy(char *dest, const char *src, size_t size);

int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);

char *strrchr(const char *s, int c);