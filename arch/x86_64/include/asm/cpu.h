#pragma once

#include <stdint.h>

struct arch_cpu {
    struct tss_entry *tss;
    uint32_t xsave_size;
    uint32_t xsave_user_size;
};
