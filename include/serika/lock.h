#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    volatile uint32_t now_serving;
    volatile uint32_t next_ticket;
    volatile int holder_cpu;
} spinlock_t;

#define SPINLOCK_INITIALIZER { .now_serving = 0, .next_ticket = 0, .holder_cpu = -1 }

void spin_lock_init(spinlock_t *lock);
void spin_lock(spinlock_t *lock);
bool spin_trylock(spinlock_t *lock);
void spin_unlock(spinlock_t *lock);
uint64_t spin_lock_irqsave(spinlock_t *lock);
void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags);
