#include <serika/arch.h>
#include <serika/buddy.h>
#include <serika/cpu.h>
#include <serika/pmap.h>
#include <serika/sched.h>
#include <serika/slab.h>
#include <serika/string.h>

#include <asm/arch.h>

/* what switch_to() pops on the first run: into user_task_entry, which irets through tf */
static void user_switch_frame(struct task *task) {
    uint64_t *sp = (uint64_t *)task->tf;

    sp--; *sp = (uintptr_t)user_task_entry; // ret
    sp--; *sp = 0; // rbp
    sp--; *sp = 0; // rbx
    sp--; *sp = 0; // r12
    sp--; *sp = 0; // r13
    sp--; *sp = 0; // r14
    sp--; *sp = 0; // r15

    task->arch.rsp = (uintptr_t)sp;
}

int arch_task_init(struct task *task, uintptr_t user_stack) {
    task->tf = (struct trapframe *)((uintptr_t)task->kstack + TASK_KSTACK_SIZE - sizeof(struct trapframe));

    memset(task->tf, 0, sizeof(*task->tf));

    task->tf->rip    = (uintptr_t)task->entry;
    task->tf->rdi    = 0;
    task->tf->rsp    = user_stack;
    task->tf->cs     = 0x23;
    task->tf->ss     = 0x1B;
    task->tf->rflags = 0x202; // IF

    user_switch_frame(task);

    if (arch_xsaves_init(task) == -1) {
        /* free is caller's duty*/
        return -1;
    }

    return 0;
}

/* fork: the child leaves the same system call with 0. fpu state and fs base are the caller's, now */
int arch_fork_init(struct task *task, const struct trapframe *parent) {
    task->tf = (struct trapframe *)((uintptr_t)task->kstack + TASK_KSTACK_SIZE - sizeof(struct trapframe));

    *task->tf = *parent;
    task->tf->rax = 0;

    user_switch_frame(task);

    if (arch_xsaves_init(task) == -1) return -1;

    /* the kernel uses no fpu: the registers still hold the caller's */
    asm volatile ("xsaves (%0)" :: "r"(task->arch.xsaves_area), "a"(-1), "d"(-1) : "memory");

    uint64_t fs;
    asm volatile ("rdfsbase %0" : "=r"(fs));
    task->arch.fs_base = fs;

    return 0;
}

/* the fpu as a new program finds it: x87 and avx at init, sse zero, mxcsr the abi's default */
void arch_fpu_reset(void) {
    /* standard form. xstate_bv has only sse: its registers and mxcsr come from here */
    static const uint8_t area[576] __attribute__((aligned(64))) = {
        [24] = 0x80, [25] = 0x1f, /* mxcsr 0x1f80 */
        [512] = 1 << 1,           /* xstate_bv */
    };

    uint32_t lo, hi;
    asm volatile ("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    asm volatile ("xrstor (%0)" :: "r"(area), "a"(lo), "d"(hi) : "memory");
}

/* execve: a fresh start at entry. no register, fs base or fpu state of the old program survives */
void arch_exec_regs(struct trapframe *tf, uintptr_t entry, uintptr_t sp) {
    memset(tf, 0, sizeof(*tf));

    tf->rip    = entry;
    tf->rsp    = sp;
    tf->cs     = 0x23;
    tf->ss     = 0x1B;
    tf->rflags = 0x202; // IF

    /* switch_to() saves fs from the register, the field alone would be lost */
    interrupt_status_t flags = arch_irq_save();
    asm volatile ("wrfsbase %0" :: "r"(0UL) : "memory");
    current_task()->arch.fs_base = 0;
    arch_irq_restore(flags);

    arch_fpu_reset();
}

int arch_kthread_init(struct task *task) {
    task->tf = (struct trapframe *)((uintptr_t)task->kstack + TASK_KSTACK_SIZE - sizeof(struct trapframe));

    memset(task->tf, 0, sizeof(*task->tf));

    task->tf->rip    = (uintptr_t)task_start;
    task->tf->rsp    = (uintptr_t)task->tf - 8;
    task->tf->cs     = 0x08;
    task->tf->ss     = 0x10;
    task->tf->rflags = 0x002; // IF off. task_start() turns it on after dropping rq lock

    uint64_t *sp = (uint64_t *)task->tf;

    sp--; *sp = (uintptr_t)task_trampoline; // ret
    sp--; *sp = 0; // rbp
    sp--; *sp = 0; // rbx
    sp--; *sp = 0; // r12
    sp--; *sp = 0; // r13
    sp--; *sp = 0; // r14
    sp--; *sp = 0; // r15

    task->arch.rsp = (uintptr_t)sp;

    return arch_xsaves_init(task);
}

int arch_xsaves_init(struct task *task) {
    task->arch.xsaves_area = kmalloc_aligned(64, curcpu->arch_cpu_data->xsave_size);

    if (!task->arch.xsaves_area) return -1;

    memset(task->arch.xsaves_area, 0, curcpu->arch_cpu_data->xsave_size);

    uint32_t *mxcsr = (uint32_t *)((uint8_t *)task->arch.xsaves_area + 24);
    *mxcsr = 0x1f80;

    uint64_t *xstate_bv = (uint64_t *)((uint8_t *)task->arch.xsaves_area + 512);
    *xstate_bv = (1ULL << 1);

    uint64_t *xcomp_bv = (uint64_t *)((uint8_t *)task->arch.xsaves_area + 520); // 512 + 8
    *xcomp_bv = (1ULL << 63) | (1ULL << 1); // compacted format. xstate_bv must be a subset

    return 0;
}

/* kgs is 0 if the last task here entered from user and got switched out in the kernel */
void arch_user_gs_init(void) {
    wrmsr(IA32_KERNEL_GS_BASE, (uint64_t)this_cpu_read(self));
    wrmsr(IA32_GS_BASE, 0);
}
