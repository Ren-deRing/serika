#pragma once

#include <serika/printk.h>
#include <stdbool.h>
#include <stdint.h>

#define TEST_ASSERT(cond)                      \
    do {                                       \
        if (!(cond)) {                         \
            printk("[FAIL] %s:%d: %s\n",       \
                   __FILE__, __LINE__, #cond); \
            return false;                      \
        }                                      \
    } while (0)

static uint32_t rand_state = 0x12345678;
static inline uint32_t rand_u32(void) {
    uint32_t x = rand_state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;

    rand_state = x;
    return x;
}

void test_run_all();

bool test_buddy_merge();
bool test_slab_reuse();
bool test_slab_multi();
bool test_slab_stress();
bool test_slab_state();
