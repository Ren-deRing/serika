#include <serika/buddy.h>
#include <serika/cpu.h>
#include <serika/pmap.h>
#include <serika/sched.h>
#include <serika/slab.h>
#include <serika/string.h>

int arch_task_init(struct task *task) {
    task->tf = (struct trapframe *)((uintptr_t)task->kstack + TASK_KSTACK_SIZE - sizeof(struct trapframe));

    memset(task->tf, 0, sizeof(*task->tf));

    task->tf->rip    = (uintptr_t)task->entry;
    task->tf->rdi    = 0;
    task->tf->rsp    = USER_STACK_TOP;
    task->tf->cs     = 0x23;
    task->tf->ss     = 0x1B;
    task->tf->rflags = 0x202; // IF

    uint64_t *sp = (uint64_t *)task->tf;

    sp--; *sp = (uintptr_t)user_task_entry; // ret
    sp--; *sp = 0; // rbp
    sp--; *sp = 0; // rbx
    sp--; *sp = 0; // r12
    sp--; *sp = 0; // r13
    sp--; *sp = 0; // r14
    sp--; *sp = 0; // r15

    task->arch.rsp = (uintptr_t)sp;

    if (arch_xsaves_init(task) == -1) {
        /* free is caller's duty*/
        return -1;
    }

    return 0;
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

    uint64_t *xcomp_bv = (uint64_t *)((uint8_t *)task->arch.xsaves_area + 520); // 512 + 8
    *xcomp_bv = (1ULL << 63); // compacted format

    return 0;
}
