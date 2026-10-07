#pragma once

#include <stdint.h>

struct intc_ops {
    const char *name;
    
    void (*eoi)(void);
    uint32_t (*get_id)(void);
    void (*send_ipi)(uint32_t dest_apic_id, uint8_t vector);
    void (*timer_stop)(void);
    void (*timer_deadline_init)(uint8_t vector); /* once per cpu */
    void (*timer_deadline)(uint64_t tsc);
    
    void (*mask_irq)(uint32_t irq);
    void (*unmask_irq)(uint32_t irq);
};

extern struct intc_ops *g_intc;

int lapic_init(void);

static inline void intc_eoi(void) {
    if (g_intc && g_intc->eoi) g_intc->eoi();
}

static inline uint32_t intc_get_id(void) {
    return g_intc ? g_intc->get_id() : 0;
}

static inline void intc_send_ipi(uint32_t dest_apic_id, uint8_t vector) {
    if (g_intc && g_intc->send_ipi) g_intc->send_ipi(dest_apic_id, vector);
}
