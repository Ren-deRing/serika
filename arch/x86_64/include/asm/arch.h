#pragma once

#include <stdint.h>

#define IA32_GS_BASE 0xC0000101

uint64_t rdmsr(uint32_t msr);
void wrmsr(uint32_t msr, uint64_t val);

void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx);

uint64_t rdtsc(void);

uintptr_t read_cr3(void);
void write_cr3(uintptr_t phys);

void flush_tlb(uintptr_t virt);
void flush_tlb_all(void);
