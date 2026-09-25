#include <serika/arch.h>
#include <serika/printk.h>

#include <asm/trapframe.h>

#include "idt.h"

#include <stdbool.h>

static volatile idt_entry_t idt[256];
static volatile idtr_t idtr;
static struct isr_slot handlers[256];

void isr_handler(struct trapframe *tf) {
    struct isr_slot *slot = &handlers[tf->vector];
    if (slot->func) { slot->func(tf, slot->data); return; }

    printk("\nPANIC! [#%02lu] [E%lu]\n", tf->vector, tf->error);
    printk("  CS:RIP = 0x%04lx:0x%016lx\n", tf->cs, tf->rip);
    printk("  RAX:     0x%016lx  RBX: 0x%016lx\n", tf->rax, tf->rbx);
    printk("  RCX:     0x%016lx  RDX: 0x%016lx\n", tf->rcx, tf->rdx);
    printk("  RSP:     0x%016lx  RBP: 0x%016lx\n", tf->rsp, tf->rbp);
    printk("  RFLAGS:  0x%016lx\n", tf->rflags);

    if (tf->vector == 14) {
        uint64_t err = tf->error;
        char *cause = (err & 0x2) ? "Write" : "Read";  // write or read
        char *priv  = (err & 0x4) ? "User" : "Kernel"; // user  or kernel
        char *type  = (err & 0x10) ? "Instruction" : "Data";

        uint64_t addr;
        asm volatile ("mov %%cr2, %0" : "=r"(addr));
        printk("\nPF:: [%s] Mode [%s] [%s] Access to [0x%016lx] failed\n", priv, cause, type, addr);
    }

    if (tf->vector != 3) {
        printk("oh no, halting.\n");
        for (;;) arch_halt();
    }

    printk("don't worry! that was not a panic.\n\n");
}

void idt_set_descriptor(uint8_t vector, void* isr, uint8_t flags, uint8_t ist) {
    uintptr_t addr = (uintptr_t)isr;
    idt[vector].isr_low    = addr & 0xFFFF;
    idt[vector].kernel_cs  = 0x08;       // GDT's kernel CS
    idt[vector].ist        = ist & 0x07; // low 3
    idt[vector].attributes = flags;
    idt[vector].isr_mid    = (addr >> 16) & 0xFFFF;
    idt[vector].isr_high   = (addr >> 32) & 0xFFFFFFFF;
    idt[vector].reserved   = 0;
}

void register_handler(uint8_t vector, handler_t handler, void *data) {
    handlers[vector].func = handler;
    handlers[vector].data = data;
}

int init_idt() {
    idt_set_descriptor(0,  isr0,  0x8E, 0);
	idt_set_descriptor(1,  isr1,  0x8E, 0);
	idt_set_descriptor(2,  isr2,  0x8E, 0);
	idt_set_descriptor(3,  isr3,  0x8E, 0);
	idt_set_descriptor(4,  isr4,  0x8E, 0);
	idt_set_descriptor(5,  isr5,  0x8E, 0);
	idt_set_descriptor(6,  isr6,  0x8E, 0);
	idt_set_descriptor(7,  isr7,  0x8E, 0);
	idt_set_descriptor(8,  isr8,  0x8E, 1); // DF IST
	idt_set_descriptor(9,  isr9,  0x8E, 0);
	idt_set_descriptor(10, isr10, 0x8E, 0);
	idt_set_descriptor(11, isr11, 0x8E, 0);
	idt_set_descriptor(12, isr12, 0x8E, 0);
	idt_set_descriptor(13, isr13, 0x8E, 0);
	idt_set_descriptor(14, isr14, 0x8E, 0);
	idt_set_descriptor(15, isr15, 0x8E, 0);
	idt_set_descriptor(16, isr16, 0x8E, 0);
	idt_set_descriptor(17, isr17, 0x8E, 0);
	idt_set_descriptor(18, isr18, 0x8E, 0);
	idt_set_descriptor(19, isr19, 0x8E, 0);
	idt_set_descriptor(20, isr20, 0x8E, 0);
	idt_set_descriptor(21, isr21, 0x8E, 0);
	idt_set_descriptor(22, isr22, 0x8E, 0);
	idt_set_descriptor(23, isr23, 0x8E, 0);
	idt_set_descriptor(24, isr24, 0x8E, 0);
	idt_set_descriptor(25, isr25, 0x8E, 0);
	idt_set_descriptor(26, isr26, 0x8E, 0);
	idt_set_descriptor(27, isr27, 0x8E, 0);
	idt_set_descriptor(28, isr28, 0x8E, 0);
	idt_set_descriptor(29, isr29, 0x8E, 0);
	idt_set_descriptor(30, isr30, 0x8E, 0);
	idt_set_descriptor(31, isr31, 0x8E, 0);

    for (int i = 0; i < (255 - 31); i++) {
        idt_set_descriptor(i + 32, (void*)isr_stub_table[i], 0x8E, 0);
    }

    return 0;
}

static volatile bool is_table_initialized = false;

void idt_init() {
    if (!is_table_initialized) init_idt(); // same thing as gdt
    is_table_initialized = true;

    idtr.limit = sizeof(idt) - 1;
	idtr.base  = (uintptr_t)&idt;

    asm volatile ("lidt %0" : : "m"(idtr));
}
