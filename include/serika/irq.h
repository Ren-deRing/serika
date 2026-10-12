#pragma once

#include <stdint.h>

struct trapframe;

typedef void (*irq_handler_t)(struct trapframe *tf, void *data);

/* the io apic. the handler eois */
void ioapic_init(void);
int  irq_request(uint32_t isa_irq, irq_handler_t handler, void *data); /* to the cpu that calls */
