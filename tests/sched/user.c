#include <serika/arch.h>
#include <serika/cpu.h>
#include <serika/sched.h>
#include <serika/syscall.h>

#include <asm/arch.h>

#include <tests/test.h>

static const uint8_t code[] = {
    0xb9, 0x00, 0x00, 0x00, 0x10,       /* mov $0x10000000, %ecx */
    0xff, 0xc9,                         /* 1: dec %ecx           */
    0x75, 0xfc,                         /* jnz 1b                */
    0xb8, 0xff, 0xff, 0x00, 0x00,       /* mov $0xffff, %eax     */
    0x0f, 0x05,                         /* syscall               */
    0x48, 0x83, 0xf8, 0xda,             /* cmp $-38, %rax        */
    0x75, 0x0c,                         /* jne 2f                */
    0xbf, 0x5e, 0x00, 0x00, 0x00,       /* mov $0x5e, %edi       */
    0xb8, 0x3c, 0x00, 0x00, 0x00,       /* mov $60, %eax (exit)  */
    0x0f, 0x05,                         /* syscall (SYS_EXIT)    */
    0xeb, 0xfe,                         /* 2: jmp 2b             */
};

static const uint8_t stack_code[] = {
    0x6a, 0x5e,                         /* push $0x5e            */
    0x5f,                               /* pop %rdi              */
    0xb8, 0x3c, 0x00, 0x00, 0x00,       /* mov $60, %eax (exit)  */
    0x0f, 0x05,                         /* syscall (SYS_EXIT)    */
    0xeb, 0xfe,                         /* jmp .                 */
};

static const uint8_t segv_code[] = {
    0x89, 0x04, 0x25, 0x00, 0x10, 0x00, 0x00, /* mov %eax, 0x1000 */
    0xeb, 0xfe,                               /* jmp .            */
};

/* reaped id, 0 on timeout */
static long run_user(const uint8_t *c, size_t size, long *status) {
    struct task *t = task_create_user(c, size, 0);
    if (!t) return 0;

    sched_add_task(t);

    long reaped = 0;
    uint64_t until = rdtsc() + curcpu->tsc_freq_hz * 2; /* 2s */

    while ((reaped = task_wait(t, status)) == 0 && rdtsc() < until) arch_pause();

    return reaped;
}

bool test_user_task() {
    long status = 0;

    TEST_ASSERT(run_user(code, sizeof(code), &status) > 0);
    TEST_ASSERT(WEXITSTATUS(status) == 0x5e);
    return true;
}

bool test_user_fault() {
    long status = 0;

    /* the stack page comes on the first push */
    TEST_ASSERT(run_user(stack_code, sizeof(stack_code), &status) > 0);
    TEST_ASSERT(WEXITSTATUS(status) == 0x5e);

    /* no vma there */
    TEST_ASSERT(run_user(segv_code, sizeof(segv_code), &status) > 0);
    TEST_ASSERT(status == 11);
    return true;
}
