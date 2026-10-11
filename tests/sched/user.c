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

bool test_user_task() {
    struct task *t = task_create_user(code, sizeof(code), 0);
    TEST_ASSERT(t);

    sched_add_task(t);

    long status = 0;
    long reaped = 0;
    uint64_t until = rdtsc() + curcpu->tsc_freq_hz * 2; /* 2s */

    while ((reaped = task_wait(t, &status)) == 0 && rdtsc() < until) arch_pause();

    TEST_ASSERT(reaped > 0);
    TEST_ASSERT(WEXITSTATUS(status) == 0x5e);
    return true;
}
