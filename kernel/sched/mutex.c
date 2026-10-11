#include <serika/cpu.h>
#include <serika/mutex.h>
#include <serika/panic.h>
#include <serika/preempt.h>
#include <serika/sched.h>
#include <serika/wait.h>

void mutex_init(struct mutex *m) {
    m->owner = NULL;
    wait_queue_init(&m->wq);
}

bool mutex_trylock(struct mutex *m) {
    struct task *expected = NULL;

    return __atomic_compare_exchange_n(&m->owner, &expected, current_task(), false,
                                       5, 0); /* __ATOMIC_SEQ_CST, __ATOMIC_RELAXED */
}

void mutex_lock(struct mutex *m) {
    struct task *curr = current_task();

    /* even uncontended: a bad caller is found before it deadlocks */
    might_sleep();

    /* only we can have stored ourselves there */
    if (__atomic_load_n(&m->owner, 0) == curr) /* __ATOMIC_RELAXED */
        panic("mutex: task %lu takes a mutex it holds -- mutex 0x%lx, caller 0x%lx",
              curr->id, (uintptr_t)m, (uintptr_t)__builtin_return_address(0));

    if (mutex_trylock(m)) return;

    wait_event(m->wq, mutex_trylock(m));
}

void mutex_unlock(struct mutex *m) {
    struct task *curr = current_task();
    struct task *owner = __atomic_load_n(&m->owner, 0); /* __ATOMIC_RELAXED */

    if (owner != curr)
        panic("mutex: task %lu unlocks a mutex owned by %ld -- mutex 0x%lx, caller 0x%lx",
              curr->id, owner ? (long)owner->id : -1L, (uintptr_t)m,
              (uintptr_t)__builtin_return_address(0));

    /* before wake_up() reads the queue: a waiter that missed this is in it */
    __atomic_store_n(&m->owner, NULL, 5); /* __ATOMIC_SEQ_CST */

    wake_up(&m->wq);
}
