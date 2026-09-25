#pragma once

#include <stdint.h>

typedef uint64_t interrupt_status_t;

interrupt_status_t arch_irq_save(void);
void arch_irq_restore(interrupt_status_t flags);

void arch_irq_disable(void);
void arch_irq_enable(void);
void arch_halt();
void arch_pause();
void arch_panic(void);
