#pragma once

#include <stdint.h>
#define MAX_CPUS 256

struct cpu {
    struct cpu *self;

    uint64_t    kstack_top;
    uint64_t    user_sp;

    uint32_t    id;
    uint32_t    hw_id;

    uint64_t    tsc_freq_hz;

    struct arch_cpu       *arch_cpu_data;

    struct thread         *current;
    struct thread         *idle;
} __attribute__((aligned(64)));

void cpu_init();

struct cpu* get_this_core(void);

#define curcpu get_this_core()
