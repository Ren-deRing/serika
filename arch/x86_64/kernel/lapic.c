#include <serika/cpu.h>
#include <serika/intc.h>
#include <serika/panic.h>

#include <asm/arch.h>
#include <asm/cpu.h> 

#define IA32_APIC_BASE         0x01B
#define IA32_TSC_DEADLINE      0x6E0

#define IA32_X2APIC_ID         0x802
#define IA32_X2APIC_VER        0x803
#define IA32_X2APIC_TPR        0x808 // task priority
#define IA32_X2APIC_EOI        0x80B
#define IA32_X2APIC_LDR        0x80D // logical destination
#define IA32_X2APIC_SIVR       0x80F
#define IA32_X2APIC_ESR        0x828 // error status
#define IA32_X2APIC_ICR        0x830 // interrupt command register
#define IA32_X2APIC_LVT_TIMER  0x832
#define IA32_X2APIC_TIMER_INIT 0x838
#define IA32_X2APIC_TIMER_CUR  0x839
#define IA32_X2APIC_TIMER_DIV  0x83E // divide configuration

#define LAPIC_TIMER_ONESHOT      (0b00 << 17)
#define LAPIC_TIMER_PERIODIC     (0b01 << 17)
#define LAPIC_TIMER_TSC_DEADLINE (0b10 << 17)

#define LAPIC_TIMER_MASKED       (1 << 16)

void x2apic_eoi(void) {
    wrmsr(IA32_X2APIC_EOI, 0);
}

void lapic_send_ipi(uint32_t dest_apic_id, uint8_t vector) {
    wrmsr(IA32_X2APIC_ICR, ((uint64_t)dest_apic_id << 32) | vector);
}

uint32_t x2apic_get_id(void) {
    return (uint32_t)rdmsr(IA32_X2APIC_ID);
}

void lapic_timer_stop(void) {
    wrmsr(IA32_X2APIC_LVT_TIMER, LAPIC_TIMER_MASKED);
}

void lapic_timer_deadline_init(uint8_t vector) {
    wrmsr(IA32_X2APIC_LVT_TIMER, LAPIC_TIMER_TSC_DEADLINE | vector);
    asm volatile ("mfence" ::: "memory");
}

/* hot path */
void lapic_timer_deadline(uint64_t tsc) {
    wrmsr(IA32_TSC_DEADLINE, tsc);
}

int lapic_init(void) {
    uint32_t eax, ebx, ecx, edx;

    cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    if (!(ecx & (1 << 21))) panic("lapic: no x2APIC -- does your cpu support x2APIC?");
    if (!(ecx & (1 << 24))) panic("lapic: no TSC-deadline -- does your cpu support TSC-deadline timer?");

    wrmsr(IA32_APIC_BASE, rdmsr(IA32_APIC_BASE) | (1ULL << 11) | (1ULL << 10)); /* xAPIC enable | x2APIC enable */

    wrmsr(IA32_X2APIC_TPR, 0);

    uint64_t sivr = rdmsr(IA32_X2APIC_SIVR);
    sivr |= (1ULL << 8) | 0xFF; 
    wrmsr(IA32_X2APIC_SIVR, sivr);

    return 0;
}

static struct intc_ops x2apic_ops = {
    .name = "x2APIC",
    .eoi = x2apic_eoi,
    .get_id = x2apic_get_id,
    .send_ipi = lapic_send_ipi,

    .timer_stop = lapic_timer_stop,
    .timer_deadline_init = lapic_timer_deadline_init,
    .timer_deadline = lapic_timer_deadline,

    .mask_irq = NULL,
    .unmask_irq = NULL,
};

struct intc_ops *g_intc = &x2apic_ops;
