#pragma once

#include <stdint.h>

struct mm;

/* waiters on mm's word at addr, n at most. how many woke */
int futex_wake(struct mm *mm, uintptr_t addr, int n);
