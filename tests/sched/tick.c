#include <serika/arch.h>
#include <serika/cpu.h>
#include <serika/tick.h>

#include <asm/arch.h>

#include <tests/test.h>

bool test_tick_rate() {
    struct cpu *c = curcpu;

    uint64_t start = *(volatile uint64_t *)&c->ticks;
    uint64_t until = rdtsc() + c->tsc_freq_hz / 20; /* 50ms */

    while (rdtsc() < until) arch_pause();

    uint64_t elapsed = *(volatile uint64_t *)&c->ticks - start;

    TEST_ASSERT(elapsed >= 45 && elapsed <= 55); /* HZ = 1000 */
    return true;
}
