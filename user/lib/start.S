/* sysv entry: argc at rsp, then argv, envp and auxv. rsp is 16 aligned */
.text
.globl _start
.type _start, @function
_start:
    testq $15, %rsp
    jnz 2f

    movq (%rsp), %rdi           /* argc */
    leaq 8(%rsp), %rsi          /* argv */
    leaq 8(%rsi,%rdi,8), %rdx   /* envp */

    movq %rdx, %rcx
1:  cmpq $0, (%rcx)
    leaq 8(%rcx), %rcx
    jne 1b                      /* rcx: auxv */

    call main

    movl %eax, %edi
    movl $60, %eax              /* exit */
    syscall

2:  movl $14, %edi              /* misaligned stack */
    movl $60, %eax
    syscall
