.code64

.macro NOERR num
.global isr\num
isr\num:
    pushq $0    /* padding */
    pushq $\num /* interrupt */
    jmp isr_common_entry
.endm

.macro ERR num
.global isr\num
isr\num:
    /* no padding here, errcode here (CPU pushes) */
    pushq $\num
    jmp isr_common_entry
.endm

NOERR  0
NOERR  1
NOERR  2
NOERR  3
NOERR  4
NOERR  5
NOERR  6
NOERR  7
ERR    8
NOERR  9
ERR   10
ERR   11
ERR   12
ERR   13
ERR   14
NOERR 15
NOERR 16
ERR   17
NOERR 18
NOERR 19
NOERR 20
ERR   21
NOERR 22
NOERR 23
NOERR 24
NOERR 25
NOERR 26
NOERR 27
NOERR 28
ERR   29
ERR   30
NOERR 31

.altmacro
i = 32
.rept 224
    NOERR %i
    i = i + 1
.endr

.macro isr_entry index
    .quad isr\index
.endm

isr_stub_table:
    i = 32
    .rept 224
        isr_entry %i
        i = i + 1
    .endr
.noaltmacro

.global isr_stub_table

.macro do_swapgs
    testq $0x3, 24(%rsp)
    jz .L_macro_exit_\@

    swapgs

.L_macro_exit_\@:
.endm

.global isr_common_entry
isr_common_entry:
    do_swapgs

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
    call isr_handler

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

    do_swapgs

    addq $16, %rsp
    iretq
