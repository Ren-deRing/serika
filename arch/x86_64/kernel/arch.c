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

void arch_panic(void) {
    /* TODO: Halt all other CPUs via IPI before entering the panic loop. */

    arch_irq_disable();
    for (;;) arch_halt();
}

uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    __asm__ __volatile__(
        "rdmsr"
        : "=a"(low), "=d"(high)
        : "c"(msr)
    );
    return ((uint64_t)high << 32) | low;
}

void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t low = (uint32_t)val;
    uint32_t high = (uint32_t)(val >> 32);
    __asm__ __volatile__(
        "wrmsr"
        :
        : "c"(msr), "a"(low), "d"(high)
    );
}

void cpuid(uint32_t leaf, uint32_t subleaf,
                                uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    asm volatile("cpuid"
                 : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                 : "a"(leaf), "c"(subleaf));
}
