#pragma once

#include <serika/buddy.h>
#include <stdint.h>

struct arch_task {
    uint64_t rsp;
    uint64_t fs_base;
    void    *xsaves_area;
};

struct task;

int arch_task_init(struct task *task, uintptr_t user_stack);
int arch_kthread_init(struct task *task);
int arch_xsaves_init(struct task *task);

struct trapframe;

int  arch_fork_init(struct task *task, const struct trapframe *parent);
void arch_exec_regs(struct trapframe *tf, uintptr_t entry, uintptr_t sp);
void arch_fpu_reset(void);

void user_task_entry();
void arch_user_gs_init(void);
void task_trampoline();
