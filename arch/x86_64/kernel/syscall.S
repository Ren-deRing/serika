.code64

#include <serika/offsets.h>

.global syscall_entry
/*
    rcx = user rip, r11 = user rflags, rsp is user one.
    interrupts are off until back in user mode.
*/
syscall_entry:
    swapgs
    movq %rsp, %gs:CPU_USER_RSP_OFFSET
    movq %gs:CPU_KSTACK_TOP_OFFSET, %rsp

    /* struct trapframe */
    pushq $0x1B                        /* ss */
    pushq %gs:CPU_USER_RSP_OFFSET      /* rsp */
    pushq %r11                         /* rflags */
    pushq $0x23                        /* cs */
    pushq %rcx                         /* rip */
    pushq $0                           /* error */
    pushq $0                           /* vector */

    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq %rsp, %rdi # trapframe
    call arch_syscall

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
    popq %rax                          /* return value */

    addq $16, %rsp                     /* vector, error */
    popq %rcx                          /* rip */

    /* canonical rip: sign-extend from bit 47 and compare. r11 is free, rax is the return value */
    movq %rcx, %r11
    shlq $16, %r11
    sarq $16, %r11
    cmpq %r11, %rcx
    jne .Lbad_rip

    addq $8, %rsp                      /* cs */
    popq %r11                          /* rflags */
    popq %rsp                          /* user rsp. */

    swapgs
    sysretq

.Lbad_rip:
    call syscall_bad_rip               /* never returns. still on the kernel stack */
