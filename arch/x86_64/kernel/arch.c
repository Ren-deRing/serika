#include <serika/arch.h>
#include <serika/compiler.h>

notrace interrupt_status_t arch_irq_save(void) {
    interrupt_status_t flags;
    asm volatile ("pushfq; pop %0; cli" : "=rm"(flags) :: "memory");
    return flags;
}

notrace void arch_irq_restore(interrupt_status_t flags) {
    asm volatile ("push %0; popfq" : : "rm"(flags) : "memory", "cc");
}

notrace void arch_irq_disable(void) {
    asm volatile ("cli");
}

notrace void arch_irq_enable(void) {
    asm volatile ("sti");
}

notrace void arch_halt() {
    asm volatile ("hlt");
}

notrace void arch_pause() {
    asm volatile ("pause");
}

