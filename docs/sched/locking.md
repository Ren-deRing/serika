# Runqueue Locking

Each CPU has one runqueue, `curcpu->rq` (`cpus[id].rq`). `rq->lock` protects its state:
`tasks`, `curr`, `dead`, `min_vruntime`, `load`, `nr_running`, and `need_resched`.

`rq->online` is set once in `sched_init()` and is read without the lock.

## Rules

- Taking `rq->lock` from a context that can take an interrupt requires `spin_lock_irqsave()`.
  The tick handler is the exception: interrupts are already off there.
- Hold at most one rq lock at a time, with one exception: `balance()` may `spin_trylock()`
  a remote rq while holding its own. A failed trylock skips the migration. Never block on
  a second rq lock.
- The lock is released by the CPU the task is running on at that moment, not the one
  that took it. `schedule()` unlocks `curcpu->rq.lock`.

## Switching tasks

`schedule()` holds the lock across `switch_to()`. The task that resumes releases it.

New kernel threads start in `task_start()` with the lock held and interrupts off. They
unlock, then enable interrupts. New user tasks start in `user_task_entry`, which calls
`task_start_user()` to unlock. `iretq` then restores interrupts from the trapframe.

## Interrupt paths

- `sched_tick()` uses `spin_lock()`, since interrupts are already off.
- `resched_handler()` calls `schedule()`, which does its own irq save and restore.
- `sched_add_task_cpu()` locks the target rq with `spin_lock_irqsave()`. It sends
  `RESCHED_VECTOR` after unlocking, and only if the target is another CPU that is idle.

## Lock-free reads

`select_cpu()` and `balance()` read other runqueues' `nr_running` without a lock. These
are hints. `balance()` locks the busiest rq with `trylock` and checks the counts again
before moving a task.

## Lock order

The rq lock is taken before the buddy and slab locks, never after. `task_free()` runs
under the rq lock and calls both allocators.
