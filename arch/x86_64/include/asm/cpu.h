#pragma once

#include <stdint.h>

struct arch_cpu {
    struct tss_entry *tss;
    uint32_t xsave_size;
};
