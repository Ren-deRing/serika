#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    char *small = malloc(64);
    if (!small) return 1;
    strcpy(small, "heap");

    volatile char *big = malloc(1 << 20);
    if (!big) return 2;
    for (int i = 0; i < (1 << 20); i += 4096) big[i] = (char)i;
    if (big[4096] != (char)4096) return 3;

    printf("hello from glibc: argc=%d argv[0]=%s %s %.2f\n", argc, argv[0], small, 3.14159);

    free((void *)big);
    free(small);
    return 0x5e;
}
