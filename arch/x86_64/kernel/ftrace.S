.code64

.global ftrace_caller
.type ftrace_caller, @function

ftrace_caller:
    /*
        Since ftrace_trace() is a 'C' function and may clobber
        caller-saved registers, preserve them before calling it.
    */

    push %rax
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %r8
    push %r9
    push %r10
    push %r11

    /*
        rsp+00  saved r11
        rsp+08  saved r10
        rsp+16  saved r9
        rsp+24  saved r8
        rsp+32  saved rdi
        rsp+40  saved rsi
        rsp+48  saved rdx
        rsp+56  saved rcx
        rsp+64  saved rax
        rsp+72  return address (= original function + 5)

        The patched call(rel32) is 5 bytes long, so the return address
        points 5 bytes past the original function's entry.
    */

    mov 72(%rsp), %rdi
    sub $5, %rdi
    call ftrace_trace

    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rax

    ret
