#include <serika/compiler.h>
#include <serika/syscall.h>
#include <serika/task.h>

#include <asm/arch.h>
#include <asm/trapframe.h>

#define IA32_EFER   0xC0000080
#define IA32_STAR   0xC0000081
#define IA32_LSTAR  0xC0000082
#define IA32_SFMASK 0xC0000084

#define EFER_SCE (1ULL << 0)
#define SFMASK_BITS ((1ULL << 9) | (1ULL << 10) | (1ULL << 8) | (1ULL << 14) | (1ULL << 18)) /* IF DF TF NT AC */

extern void syscall_entry(void);

/* every cpu, after gdt_init() */
void syscall_init(void) {
    wrmsr(IA32_EFER, rdmsr(IA32_EFER) | EFER_SCE);

    /*
        syscall: cs = 0x08, ss = 0x10.
        sysret:  ss = 0x10 + 8 = 0x18 (user data), cs = 0x10 + 16 = 0x20 (user code). rpl 3 is added by cpu.
    */
    wrmsr(IA32_STAR, (0x10ULL << 48) | (0x08ULL << 32));
    wrmsr(IA32_LSTAR, (uint64_t)syscall_entry);
    wrmsr(IA32_SFMASK, SFMASK_BITS);
}

/* kill the task instead */
notrace void syscall_bad_rip(void) {
    task_exit(11); /* no signals yet */
}

/* called from syscall_entry, irq off */
notrace void arch_syscall(struct trapframe *tf) {
    const long args[6] = { tf->rdi, tf->rsi, tf->rdx, tf->r10, tf->r8, tf->r9 };

    tf->rax = syscall_dispatch(tf->rax, args);
}
