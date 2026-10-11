#pragma once

#include <stddef.h>
#include <stdint.h>

#define __percpu_gs __attribute__((address_space(256)))

#define __percpu_type(member) __typeof__(((struct cpu *)0)->member)
#define __percpu_ref(member)  (*(volatile __percpu_type(member) __percpu_gs *)offsetof(struct cpu, member))

#define this_cpu_read(member)       __percpu_ref(member)
#define this_cpu_write(member, val) (__percpu_ref(member) = (val))

#define this_cpu_add(member, val)                                                       \
    do {                                                                                \
        _Static_assert(sizeof(__percpu_type(member)) == 4 ||                            \
                       sizeof(__percpu_type(member)) == 8, "this_cpu_add: size");       \
                                                                                        \
        if (sizeof(__percpu_type(member)) == 4)                                         \
            asm volatile ("addl %1, %%gs:%c0" :: "i"(offsetof(struct cpu, member)),     \
                          "ri"((uint32_t)(val)) : "memory", "cc");                      \
        else                                                                            \
            asm volatile ("addq %1, %%gs:%c0" :: "i"(offsetof(struct cpu, member)),     \
                          "re"((uint64_t)(val)) : "memory", "cc");                      \
    } while (0)
