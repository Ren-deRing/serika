#include <sys.h>

/* exits 0x5e if the loader did everything right, else the number of the failed check */

/* hidden: a pie reaches it without a got entry. nothing relocates us */
extern char _start[] __attribute__((visibility("hidden")));

static volatile int data_word = 0x1234;
static char bss_buf[3 * 4096];
static const char rodata[] = "serika";

int main(int argc, char **argv, char **envp, unsigned long *auxv) {
    if (argc != 3) return 1;
    if (!streq(argv[1], "foo") || !streq(argv[2], "bar")) return 2;
    if (argv[3]) return 3;
    if (!envp[0] || !streq(envp[0], "X=1") || envp[1]) return 4;

    unsigned long pagesz = 0, phdr = 0, phent = 0, phnum = 0, entry = 0, random = 0, execfn = 0;

    for (unsigned long *a = auxv; a[0] != AT_NULL; a += 2) {
        switch (a[0]) {
            case AT_PAGESZ: pagesz = a[1]; break;
            case AT_PHDR:   phdr   = a[1]; break;
            case AT_PHENT:  phent  = a[1]; break;
            case AT_PHNUM:  phnum  = a[1]; break;
            case AT_ENTRY:  entry  = a[1]; break;
            case AT_RANDOM: random = a[1]; break;
            case AT_EXECFN: execfn = a[1]; break;
        }
    }

    if (pagesz != 4096) return 5;
    if (!random) return 6;
    if (!phdr || phent != 56 || !phnum) return 7;

    /* the phdrs are mapped: one of them is PT_LOAD */
    int loads = 0;
    for (unsigned long i = 0; i < phnum; i++)
        if (*(unsigned int *)(phdr + i * phent) == 1) loads++;
    if (!loads) return 8;

    if (entry != (unsigned long)_start) return 9;
    if (!execfn || !streq((const char *)execfn, argv[0])) return 10;

    /* bss is zero and writable, data has its value and is writable, rodata is there */
    for (unsigned long i = 0; i < sizeof(bss_buf); i++)
        if (bss_buf[i]) return 11;
    for (unsigned long i = 0; i < sizeof(bss_buf); i++)
        bss_buf[i] = (char)i;

    if (data_word != 0x1234) return 12;
    data_word = 5;

    if (!streq(rodata, "serika")) return 13;

    return 0x5e;
}
