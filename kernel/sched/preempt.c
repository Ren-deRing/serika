#include <serika/arch.h>
#include <serika/panic.h>
#include <serika/preempt.h>
#include <serika/sched.h>

#include <stdint.h>

void __might_sleep(const char *file, int line) {
    uint32_t count = preempt_count();

    if (!(count & (PREEMPT_MASK | HARDIRQ_MASK)) && !arch_irqs_disabled()) return;

    panic("sched: sleeping in atomic context at %s:%d -- preempt_count=0x%x, irq %s",
          file, line, count, arch_irqs_disabled() ? "off" : "on");
}

void schedule_check(uintptr_t caller) {
    uint32_t count = preempt_count();

    if (count) panic("sched: scheduling while atomic -- preempt_count=0x%x, caller 0x%lx", count, caller);
}

void preempt_schedule(void) {
    if (preempt_count() != HARDIRQ_OFFSET) return;

    __schedule(true);
}
