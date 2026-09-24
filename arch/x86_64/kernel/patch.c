#include <serika/compiler.h>
#include <serika/string.h>

#include <stddef.h>
#include <stdint.h>

notrace void text_patch(void *addr, const void *opcode, size_t len) {
    uintptr_t cr0;

    asm volatile (
        "mov %%cr0, %0"
        : "=r"(cr0)
    );

    asm volatile (
        "mov %0, %%cr0"
        :
        : "r"(cr0 & ~(1UL << 16))
        : "memory"
    );

    memcpy(addr, opcode, len);

    asm volatile("mfence" ::: "memory");

    asm volatile (
        "mov %0, %%cr0"
        :
        : "r"(cr0)
        : "memory"
    );
}