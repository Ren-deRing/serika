#pragma once

#include <stddef.h>
#include <stdint.h>

typedef enum {
    MMAP_FREE = 0,
    MMAP_RESERVED,
    MMAP_ACPI_RECLAIMABLE,
    MMAP_ACPI_NVS,
    MMAP_BAD_MEMORY,
    MMAP_BOOTLOADER_RECLAIM,
    MMAP_KERNEL_AND_MODULES,
    MMAP_FRAMEBUFFER
} mtype_t;

typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t type;
} mregion_t;

typedef struct {
    void* fb_addr;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
} fbinfo_t;

typedef struct {
    uint32_t logic_id;
    uint32_t hw_id;        // x86_64: Local APIC ID / AArch64: MPIDR
    void* boot_stack_ptr;
    void* extra_info;      // reserved
} coreinfo_t;

typedef struct {
    /* Memory */
    mregion_t* mmap;
    struct {
        uint64_t length;
        uint64_t hhdm_offset;
        uint64_t max_phys_addr;
    } mem;

    /* Graphics */
    fbinfo_t fb;

    /* Kernel Binary */
    struct {
        uintptr_t phys_base;
        uintptr_t virt_base;
        void* file_ptr;          // Binery Address
        uint64_t  file_size;     // Binery Size
    } kernel;

    /* Multi-Processor */
    struct {
        uint32_t  total_cores;
        coreinfo_t* cores;
        uint32_t  bsp_hw_id;
    } smp;

    /* Initial RAM Disk */
    struct {
        uintptr_t phys_base;
        uintptr_t virt_base;
        uint64_t size;
    } initrd;

    /* System Tables */
    uintptr_t rsdp_address;
} bootinfo_t;

extern bootinfo_t g_bootinfo;
