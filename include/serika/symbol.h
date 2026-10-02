#pragma once

#include <stddef.h>
#include <stdint.h>

struct ksymbol {
    uintptr_t addr;
    uint32_t size;
    uint32_t name_offset;
};

extern const struct ksymbol ksymbols[];
extern const size_t ksymbol_count;
extern const char ksymbol_names[];

const struct ksymbol *ksymbol_find(uintptr_t addr);
const char *ksymbol_name(const struct ksymbol *sym);
