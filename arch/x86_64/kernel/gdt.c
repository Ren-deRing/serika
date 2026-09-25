#include <asm/cpu.h>
#include <serika/arch.h>
#include <serika/cpu.h>
#include <serika/printk.h>

#include <asm/trapframe.h>

#include "gdt.h"

#include <stdbool.h>

GDT gdt[MAX_CPUS];

static inline void set_gdt_entry(gdt_entry_t* entry, uint8_t access, uint8_t flags) {
    entry->limit_low = 0xFFFF;
    entry->base_low = 0;
    entry->base_mid = 0;
    entry->access = access;
    entry->flags = flags;
    entry->base_high = 0;
}

static inline void gdt_load(gdtr_t* ptr) {
    asm volatile (
        "lgdt %0\n"
        "pushq $0x08\n"           // kcode
        "leaq 1f(%%rip), %%rax\n" // label to return
        "push %%rax\n"
        "lretq\n"                 // load
        "1:\n"
        "mov $0x10, %%ax\n"       // kdata
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%ss\n"
        "mov $0x28, %%ax\n"       // tss
        "ltr %%ax\n"
        : : "m"(*ptr) : "rax", "memory"
    );
}

int init_gdt() {
    extern struct arch_cpu arch_cpus[];

    for (int i = 0; i < MAX_CPUS; i++) {
        set_gdt_entry(&gdt[i].entries[1], 0x9A, 0x20); // kernel code (0x08)
        set_gdt_entry(&gdt[i].entries[2], 0x92, 0x00); // kernel data (0x10)
        set_gdt_entry(&gdt[i].entries[3], 0xF3, 0x00); // user data   (0x1B)
        set_gdt_entry(&gdt[i].entries[4], 0xFA, 0x20); // user code   (0x23)

        // TSS
        tss_descriptor_t* tss_desc = (tss_descriptor_t*)&gdt[i].entries[5];

        uint64_t tss_base = (uint64_t)&gdt[i].tss;
        uint32_t tss_limit = sizeof(tss_entry_t) - 1;

        tss_desc->length       = tss_limit & 0xFFFF;
        tss_desc->base_low     = tss_base & 0xFFFF;
        tss_desc->base_middle1 = (tss_base >> 16) & 0xFF;
        tss_desc->flags1       = 0x89; // present, 64-bits TSS (avail)
        tss_desc->flags2       = (tss_limit >> 16) & 0x0F;
        tss_desc->base_middle2 = (tss_base >> 24) & 0xFF;
        tss_desc->base_high    = (tss_base >> 32) & 0xFFFFFFFF;
        tss_desc->reserved     = 0;

        // IST1 (for #DF)
        gdt[i].tss.ist[0] = (uintptr_t)gdt[i].df_stack + sizeof(gdt[i].df_stack);

        // kstack
        gdt[i].tss.rsp[0] = (uintptr_t)gdt[i].kstack + sizeof(gdt[i].kstack);

        arch_cpus[i].tss = &gdt[i].tss;

        // LGDT
        gdt[i].pointer.limit = sizeof(gdt[i].entries) - 1;
        gdt[i].pointer.base  = (uintptr_t)&gdt[i].entries[0];
    }

    return 0;
}

static volatile bool is_table_initialized = false;

void gdt_init() {
    /* BSP is not initialized simultaneously with the APs, volatile is sufficient. */
    if (!is_table_initialized) init_gdt();
    is_table_initialized = true;

    gdt_load(&gdt[curcpu->id].pointer);
}
