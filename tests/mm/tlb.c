#include <serika/arch.h>
#include <serika/cpu.h>
#include <serika/mm.h>
#include <serika/pmap.h>
#include <serika/sched.h>
#include <serika/syscall.h>

#include <asm/arch.h>

#include <tests/test.h>

/* every cpu shoots down every cpu at once. a deadlock panics on the timeout */
#define TLB_THREADS 4
#define TLB_ROUNDS  200

static volatile uint32_t tlb_finished;
static volatile bool     tlb_done;

static void tlb_worker(void) {
    for (int i = 0; i < TLB_ROUNDS; i++)
        pmap_shootdown(pmap_kernel(), 0xffff800000000000ULL, (i % 2) ? PAGE_SIZE : 64 * PAGE_SIZE);

    if (__atomic_add_fetch(&tlb_finished, 1, 5) == TLB_THREADS) tlb_done = true;
}

bool test_tlb_storm() {
    tlb_finished = 0;
    tlb_done = false;

    for (int i = 0; i < TLB_THREADS; i++) {
        struct task *t = task_create(tlb_worker, 0);
        TEST_ASSERT(t);
        sched_add_task(t);
    }

    TEST_ASSERT(wait_for(&tlb_done, 3000));
    return true;
}

#define TLB_PAGE 0x200000ULL

static const uint8_t spin_write[] = {
    0xc7, 0x04, 0x25, 0x00, 0x00, 0x20, 0x00, /* 1: movl $1, 0x200000 */
    0x01, 0x00, 0x00, 0x00,
    0xeb, 0xf3,                               /* jmp 1b              */
};

/* it writes on another cpu with a writable entry cached. only a shootdown stops it */
bool test_tlb_mprotect() {
    struct task *t = task_create_user(spin_write, sizeof(spin_write), 0);
    TEST_ASSERT(t);
    TEST_ASSERT(mm_map(t->mm, TLB_PAGE, PAGE_SIZE, PROT_READ | PROT_WRITE) == 0);

    sched_add_task(t);

    /* it is writing once the page is there. t is ours until reaped */
    uint64_t until = rdtsc() + curcpu->tsc_freq_hz / 10; /* 100ms */
    while (!pmap_extract(t->mm->pmap, TLB_PAGE) && rdtsc() < until) arch_pause();
    TEST_ASSERT(pmap_extract(t->mm->pmap, TLB_PAGE));

    TEST_ASSERT(mm_protect(t->mm, TLB_PAGE, PAGE_SIZE, PROT_READ) == 0);

    long status = 0, reaped = 0;
    until = rdtsc() + curcpu->tsc_freq_hz; /* 1s */

    while ((reaped = task_wait(t, &status, WNOHANG)) == 0 && rdtsc() < until) arch_pause();

    TEST_ASSERT(reaped > 0);
    TEST_ASSERT(status == 11);
    return true;
}
