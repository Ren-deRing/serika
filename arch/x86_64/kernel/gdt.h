#pragma once

#include <stdint.h>

typedef struct {
	uint16_t limit_low;
	uint16_t base_low;
	uint8_t base_mid;
	uint8_t access;
	uint8_t flags;
	uint8_t base_high;
} __attribute__((packed)) gdt_entry_t;

typedef struct {
    uint16_t length;
    uint16_t base_low;
    uint8_t  base_middle1;
    uint8_t  flags1;
    uint8_t  flags2;
    uint8_t  base_middle2;
    uint32_t base_high;
    uint32_t reserved;
} __attribute__((packed)) tss_descriptor_t;

typedef struct tss_entry {
	uint32_t reserved;
	uint64_t rsp[3];
	uint32_t reserved1;
	uint64_t ist[7];
	uint32_t reserved2;
	uint32_t reserved3;
	uint16_t iomap_base;
    uint16_t reserved4;
} __attribute__ ((packed)) tss_entry_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) gdtr_t;

typedef struct {
    gdt_entry_t entries[7]; // NULL, 0Code, 0Data, 3Code, 3Data, TSS LO, TSS HI, TSS
    gdtr_t      pointer;
    tss_entry_t tss;
    uint8_t df_stack[8192] __attribute__((aligned(16)));
    uint8_t kstack[16384] __attribute__((aligned(16))); 
} __attribute__((packed)) GDT;
