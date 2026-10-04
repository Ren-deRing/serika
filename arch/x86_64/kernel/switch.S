.code64

#include <serika/offsets.h>

.global switch_to
/* void switch_to(struct task *prev, struct task *next); */

switch_to:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    /* switch rsp */
    movq %rsp, (TASK_ARCH_OFFSET + ARCH_TASK_RSP_OFFSET)(%rdi) // prev.arch.rsp <= curr.arch.rsp
    movq (TASK_ARCH_OFFSET + ARCH_TASK_RSP_OFFSET)(%rsi), %rsp // curr.arch.rsp <= next->arch.rsp

    /* fsgsbase */
    rdfsbase %rax
    movq %rax, (TASK_ARCH_OFFSET + ARCH_TASK_FS_BASE_OFFSET)(%rdi) // prev->arch.fs_base = rdfsbase()
    
    movq (TASK_ARCH_OFFSET + ARCH_TASK_FS_BASE_OFFSET)(%rsi), %rax
    wrfsbase %rax                                                  // wrfsbase(next->arch.fs_base)

    /* xsaves */
    movl $0xFFFFFFFF, %eax
    movl $0xFFFFFFFF, %edx

    movq (TASK_ARCH_OFFSET + ARCH_TASK_XSAVES_AREA_OFFSET)(%rdi), %rbx // prev->arch.xsaves_area
    xsaves (%rbx)

    movq (TASK_ARCH_OFFSET + ARCH_TASK_XSAVES_AREA_OFFSET)(%rsi), %rbx // next->arch.xsaves_area
    xrstors (%rbx)

    /* switch address space */
    movq TASK_PMAP_OFFSET(%rsi), %rbx
    movq PMAP_ROOT_PHYS_OFFSET(%rbx), %rcx
    movq %cr3, %rdx
    cmpq %rcx, %rdx
    
    je .Lskip_cr3_switch
    movq %rcx, %cr3

.Lskip_cr3_switch:
    movq TASK_KSTACK_OFFSET(%rsi), %rax     // rsi = next
    addq $16384, %rax                       // kstack_size
    movq %rax, %gs:CPU_KSTACK_TOP_OFFSET

    movq %gs:CPU_ARCH_CPU_DATA_OFFSET, %rcx
    movq ARCH_CPU_TSS_OFFSET(%rcx), %rcx 
    movq %rax, 4(%rcx)                      // tss->rsp0 = kstack_top
    /* for a now. */

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp

    ret

.global user_trampoline
user_trampoline:
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax

    addq $16, %rsp

    iretq
