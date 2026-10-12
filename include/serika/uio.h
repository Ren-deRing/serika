#pragma once

#include <stdint.h>

struct iovec {
    uint64_t base;
    uint64_t len;
};

#define IOV_MAX 1024
