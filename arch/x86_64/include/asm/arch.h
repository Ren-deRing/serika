#pragma once

#include <stdint.h>

uint64_t rdmsr(uint32_t msr);
void wrmsr(uint32_t msr, uint64_t val);

void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx);
