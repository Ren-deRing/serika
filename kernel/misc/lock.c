#include <serika/arch.h>
#include <serika/cpu.h>
#include <serika/lock.h>
#include <serika/panic.h>
#include <serika/preempt.h>

void spin_lock_init(spinlock_t *lock) {
    lock->now_serving = 0;
    lock->next_ticket = 0;
    lock->owner = 0;
}

void spin_lock(spinlock_t *lock) {
    uint32_t me = this_cpu_read(id) + 1;

    preempt_count_add(1);

    /* the owner can't be running this, it is a deadlock */
    if (lock->owner == me)
        panic("lock: cpu %u takes a lock it holds -- lock 0x%lx, caller 0x%lx",
              me - 1, (uintptr_t)lock, (uintptr_t)__builtin_return_address(0));

    uint32_t ticket = __sync_fetch_and_add(&lock->next_ticket, 1);

    while (lock->now_serving != ticket) {
        arch_pause();
    }

    __sync_synchronize();

    lock->owner = me;
}

bool spin_trylock(spinlock_t *lock) {
    uint32_t me = this_cpu_read(id) + 1;

    preempt_count_add(1);

    uint32_t serving = lock->now_serving;
    if (lock->next_ticket == serving &&
        __sync_bool_compare_and_swap(&lock->next_ticket, serving, serving + 1)) {
        __sync_synchronize();

        lock->owner = me;
        return true;
    }

    preempt_count_sub(1);
    return false;
}

void spin_unlock(spinlock_t *lock) {
    uint32_t me = this_cpu_read(id) + 1;

    if (lock->owner != me)
        panic("lock: cpu %u unlocks a lock owned by %u -- lock 0x%lx, caller 0x%lx",
              me - 1, lock->owner ? lock->owner - 1 : (uint32_t)-1, (uintptr_t)lock,
              (uintptr_t)__builtin_return_address(0));

    if (!(preempt_count() & PREEMPT_MASK))
        panic("lock: unlock with no lock counted -- lock 0x%lx, caller 0x%lx",
              (uintptr_t)lock, (uintptr_t)__builtin_return_address(0));

    lock->owner = 0;

    __sync_synchronize();

    lock->now_serving++;

    preempt_count_sub(1);
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
