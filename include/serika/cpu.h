#pragma once

#include <serika/sched.h>

#include <stdint.h>

#define MAX_CPUS 256

struct cpu {
    struct cpu *self;

    uint64_t    kstack_top;

    uint32_t    id;
    uint32_t    hw_id;

    uint64_t    tsc_freq_hz;

    uint64_t    ticks;
    uint64_t    next_deadline; /* tsc */

    struct arch_cpu     *arch_cpu_data;

    struct runqueue      rq;
    
    struct task         *current;
    struct task         *idle;
} __attribute__((aligned(64)));

extern struct cpu cpus[MAX_CPUS];

void cpu_init();

struct cpu* get_this_core(void);

#define curcpu get_this_core()
