#pragma once

#include <stdint.h>

struct trapframe {
    uintptr_t r15, r14, r13, r12, r11, r10, r9, r8;
    uintptr_t rbp, rdi, rsi, rdx, rcx, rbx, rax;

    uint64_t vector;
    uint64_t error;

    uintptr_t rip;
    uintptr_t cs;
    uintptr_t rflags;
    uintptr_t rsp;
    uintptr_t ss;
};
