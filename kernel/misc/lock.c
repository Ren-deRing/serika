#include <serika/arch.h>
#include <serika/lock.h>

void spin_lock_init(spinlock_t *lock) {
    lock->now_serving = 0;
    lock->next_ticket = 0;
    lock->holder_cpu = -1;
}

void spin_lock(spinlock_t *lock) {
    uint32_t ticket = __sync_fetch_and_add(&lock->next_ticket, 1);
    
    while (lock->now_serving != ticket) {
        arch_pause();
    }

    __sync_synchronize();
}

bool spin_trylock(spinlock_t *lock) {
    uint32_t serving = lock->now_serving;
    if (lock->next_ticket != serving) {
        return false;
    }
    if (__sync_bool_compare_and_swap(&lock->next_ticket, serving, serving + 1)) {
        __sync_synchronize();
        return true;
    }
    return false;
}

void spin_unlock(spinlock_t *lock) {
    __sync_synchronize();

    lock->now_serving++;
}

uint64_t spin_lock_irqsave(spinlock_t *lock) {
    uint64_t flags = arch_irq_save();
    spin_lock(lock);
    return flags;
}

void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags) {
    spin_unlock(lock);
    arch_irq_restore(flags);
}
