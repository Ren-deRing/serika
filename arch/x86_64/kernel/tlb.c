#include <serika/arch.h>
#include <serika/cpu.h>
#include <serika/intc.h>
#include <serika/mutex.h>
#include <serika/page.h>
#include <serika/panic.h>
#include <serika/pmap.h>
#include <serika/compiler.h>

#include <asm/arch.h>
#include <asm/trapframe.h>

#define TLB_VECTOR      0x22
#define TLB_RANGE_PAGES 32   /* more than this: the whole tlb */
#define TLB_TIMEOUT_MS  1000 /* a cpu with irq off this long is a bug */

static struct mutex       tlb_mutex;  /* one shootdown in flight */
static volatile uintptr_t tlb_virt;
static volatile size_t    tlb_size;
static uint64_t           tlb_pending[MAX_CPUS / 64]; /* cpus yet to flush. atomic */

notrace static void tlb_handler(struct trapframe *tf, void *data) {
    (void)tf;
    (void)data;

    uint32_t id = this_cpu_read(id);

    if (tlb_size > TLB_RANGE_PAGES * PAGE_SIZE) {
        flush_tlb_all();
    } else {
        for (uintptr_t va = tlb_virt; va < tlb_virt + tlb_size; va += PAGE_SIZE) flush_tlb(va);
    }

    /* after this the initiator may reuse tlb_virt and tlb_size */
    __atomic_and_fetch(&tlb_pending[id / 64], ~(1ULL << (id % 64)), 5); /* __ATOMIC_SEQ_CST */

    intc_eoi();
}

/* pmap_init() */
void tlb_init(void) {
    mutex_init(&tlb_mutex);

    extern void register_handler(uint8_t vector, void (*handler)(struct trapframe *, void *), void *data);
    register_handler(TLB_VECTOR, tlb_handler, NULL);
}

/* __schedule(), irq off, prev->pmap != next->pmap. switch_to() loads next's cr3 after this */
void pmap_switch(struct pmap *prev, struct pmap *next) {
    uint32_t id = this_cpu_read(id);
    uint64_t bit = 1ULL << (id % 64);

    __atomic_and_fetch(&prev->cpus[id / 64], ~bit, 5); /* __ATOMIC_SEQ_CST */
    __atomic_or_fetch(&next->cpus[id / 64], bit, 5);
}

/* no spinlock */
void pmap_shootdown(struct pmap *pmap, uintptr_t virt, size_t size) {
    mutex_lock(&tlb_mutex);

    tlb_virt = virt;
    tlb_size = size;

    __atomic_thread_fence(5); /* __ATOMIC_SEQ_CST */

    for (int w = 0; w < MAX_CPUS / 64; w++) {
        uint64_t mask = __atomic_load_n(&pmap->cpus[w], 5); /* __ATOMIC_SEQ_CST */

        /* the kernel half is in every pmap */
        if (pmap == pmap_kernel()) {
            mask = 0;
            for (int b = 0; b < 64; b++)
                if (__atomic_load_n(&cpus[w * 64 + b].rq.online, 2)) mask |= 1ULL << b; /* __ATOMIC_ACQUIRE */
        }

        __atomic_store_n(&tlb_pending[w], mask, 5); /* __ATOMIC_SEQ_CST */
    }

    __atomic_thread_fence(5); /* __ATOMIC_SEQ_CST */

    /* ourselves too, if there: we may have migrated, the ipi finds whoever has it */
    for (int w = 0; w < MAX_CPUS / 64; w++) {
        uint64_t mask = __atomic_load_n(&tlb_pending[w], 5); /* __ATOMIC_SEQ_CST */

        for (int b = 0; b < 64; b++)
            if (mask & (1ULL << b)) intc_send_ipi(cpus[w * 64 + b].hw_id, TLB_VECTOR);
    }

    /* irq on: another shootdown's ipi reaches us while we wait */
    uint64_t until = rdtsc() + this_cpu_read(tsc_freq_hz) / 1000 * TLB_TIMEOUT_MS;

    for (int w = 0; w < MAX_CPUS / 64; w++) {
        uint64_t mask;

        while ((mask = __atomic_load_n(&tlb_pending[w], 5)) != 0) { /* __ATOMIC_SEQ_CST */
            if (rdtsc() > until)
                panic("tlb: shootdown timed out -- cpu %d still pending, caller 0x%lx",
                      w * 64 + __builtin_ctzll(mask), (uintptr_t)__builtin_return_address(0));

            arch_pause();
        }
    }

    mutex_unlock(&tlb_mutex);
}
