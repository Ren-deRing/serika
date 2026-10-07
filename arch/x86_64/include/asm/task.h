#pragma once

#include <serika/buddy.h>
#include <stdint.h>

struct arch_task {
    uint64_t rsp;
    uint64_t fs_base;
    void    *xsaves_area;
};

struct task;

int arch_task_init(struct task *task);
int arch_kthread_init(struct task *task);
int arch_xsaves_init(struct task *task);

void task_trampoline();
