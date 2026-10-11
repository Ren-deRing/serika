#include <serika/arch.h>
#include <serika/boot.h>
#include <serika/cpu.h>
#include <serika/pmap.h>
#include <serika/sched.h>
#include <serika/tick.h>

#include <asm/arch.h>

static volatile bool ap_go = false;

void smp_release_aps(void) {
    __atomic_store_n(&ap_go, true, 5); /* __ATOMIC_SEQ_CST */
}

void start_ap(struct core* info) {
    (void)info;

    while (!__atomic_load_n(&ap_go, 5)) arch_pause();

    pmap_activate(pmap_kernel());
    flush_tlb_all();

    cpu_init();
    sched_init();
    tick_init();

    task_exit(0); /* idle takes over */
}
