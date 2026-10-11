#pragma once

#include <serika/cpu.h>
#include <serika/percpu.h>

/*
    follows the task: __schedule() saves it before switch_to(), loads the next task's
    low 16 bits: spinlocks held, high 16 bits: hardirq nesting. non-zero: atomic
*/
#define PREEMPT_MASK   0x0000ffffu
#define HARDIRQ_OFFSET 0x00010000u
#define HARDIRQ_MASK   0xffff0000u

static inline uint32_t preempt_count(void) {
    return this_cpu_read(preempt_count);
}

static inline void preempt_count_set(uint32_t v) {
    this_cpu_write(preempt_count, v);
}

static inline void preempt_count_add(uint32_t v) {
    this_cpu_add(preempt_count, v);
}

static inline void preempt_count_sub(uint32_t v) {
    this_cpu_add(preempt_count, -v);
}

#define in_interrupt() ((preempt_count() & HARDIRQ_MASK) != 0)

void schedule_check(uintptr_t caller);
void __might_sleep(const char *file, int line);

/* panics in an interrupt handler, with a spinlock held, or with irq off */
#define might_sleep() __might_sleep(__FILE__, __LINE__)
