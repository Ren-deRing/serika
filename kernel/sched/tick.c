#include <serika/arch.h>
#include <serika/compiler.h>
#include <serika/cpu.h>
#include <serika/intc.h>
#include <serika/sched.h>
#include <serika/tick.h>
#include <serika/timer.h>

#include <asm/arch.h>
#include <asm/trapframe.h>

#include <stddef.h>

notrace static void tick_handler(struct trapframe *tf, void *data) {
    (void)tf;
    (void)data;

    struct cpu *c = curcpu;
    uint64_t period = c->tsc_freq_hz / HZ;

    c->ticks++;
    c->next_deadline += period;

    uint64_t now = rdtsc();
    if (c->next_deadline <= now) c->next_deadline = now + period; /* resync. */

    g_intc->timer_deadline(c->next_deadline);

    sched_tick();

    if (c->id == 0) timer_tick();

    intc_eoi();

    if (c->rq.need_resched) preempt_schedule();
}

void tick_init(void) {
    extern void register_handler(uint8_t vector, void (*handler)(struct trapframe *, void *), void *data);
    register_handler(TIMER_VECTOR, tick_handler, NULL);

    struct cpu *c = curcpu;

    g_intc->timer_deadline_init(TIMER_VECTOR);

    c->next_deadline = rdtsc() + c->tsc_freq_hz / HZ;
    g_intc->timer_deadline(c->next_deadline);

    arch_irq_enable();
}
