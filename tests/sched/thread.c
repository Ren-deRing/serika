#include <serika/arch.h>
#include <serika/cpu.h>
#include <serika/mm.h>
#include <serika/sched.h>
#include <serika/syscall.h>

#include <asm/arch.h>

#include <tests/test.h>

#define THREAD_SHARED 0x200000ULL
#define THREAD_STACK  0x300000ULL

static const uint8_t thread_code[] = {
    /* process: spin until the thread writes the shared word, exit with it */
    0x8b, 0x04, 0x25, 0x00, 0x00, 0x20, 0x00, /* 1: mov 0x200000, %eax   */
    0x85, 0xc0,                               /* test %eax, %eax         */
    0x74, 0xf5,                               /* jz 1b                   */
    0x89, 0xc7,                               /* mov %eax, %edi          */
    0xb8, 0x3c, 0x00, 0x00, 0x00,             /* mov $60, %eax (exit)    */
    0x0f, 0x05,                               /* syscall (SYS_EXIT)      */
    0xeb, 0xfe,                               /* jmp .                   */
    [0x40] =
    /* thread: write the shared word, exit(0) through its own stack */
    0xc7, 0x04, 0x25, 0x00, 0x00, 0x20, 0x00, /* movl $0x5e, 0x200000    */
    0x5e, 0x00, 0x00, 0x00,
    0x6a, 0x00,                               /* push $0                 */
    0x5f,                                     /* pop %rdi                */
    0xb8, 0x3c, 0x00, 0x00, 0x00,             /* mov $60, %eax (exit)    */
    0x0f, 0x05,                               /* syscall (SYS_EXIT)      */
    0xeb, 0xfe,                               /* jmp .                   */
};

bool test_thread_mm() {
    struct task *proc = task_create_user(thread_code, sizeof(thread_code), 0);
    TEST_ASSERT(proc);

    /* before either runs: the process reads the shared word right away */
    TEST_ASSERT(mm_map(proc->mm, THREAD_SHARED, PAGE_SIZE, PROT_READ | PROT_WRITE) == 0);
    TEST_ASSERT(mm_map(proc->mm, THREAD_STACK, 16 * PAGE_SIZE, PROT_READ | PROT_WRITE) == 0);

    struct task *thread = task_create_thread(proc, USER_CODE_BASE + 0x40, THREAD_STACK + 16 * PAGE_SIZE, 0);
    TEST_ASSERT(thread);
    TEST_ASSERT(thread->mm == proc->mm);

    sched_add_task(proc);
    sched_add_task(thread);

    long proc_status = 0, thread_status = 0;
    long proc_id = 0, thread_id = 0;
    uint64_t until = rdtsc() + curcpu->tsc_freq_hz * 2; /* 2s */

    while ((!proc_id || !thread_id) && rdtsc() < until) {
        if (!proc_id)   proc_id = task_wait(proc, &proc_status, WNOHANG);
        if (!thread_id) thread_id = task_wait(thread, &thread_status, WNOHANG);
        arch_pause();
    }

    TEST_ASSERT(proc_id > 0 && thread_id > 0);
    TEST_ASSERT(WEXITSTATUS(proc_status) == 0x5e);
    TEST_ASSERT(WEXITSTATUS(thread_status) == 0);
    return true;
}

