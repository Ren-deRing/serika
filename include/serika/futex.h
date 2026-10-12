#pragma once

#include <stdint.h>

struct mm;
struct task;

/* waiters on mm's word at addr, n at most. how many woke */
void futex_exit_robust(struct task *task); /* the task itself, mm still there */
int futex_wake(struct mm *mm, uintptr_t addr, int n);
