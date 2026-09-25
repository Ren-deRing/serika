#pragma once

#include <stdint.h>

struct arch_cpu {
    struct tss_entry *tss;
    uint32_t xsave_size;
};

// uint64_t rdmsr(uint32_t msr);
// void wrmsr(uint32_t msr, uint64_t val);
