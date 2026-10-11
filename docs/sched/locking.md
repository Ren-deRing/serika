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
- Remote rq locks are taken only by `sched_add_task_cpu()` and by the read helpers
  (`sched_nr_running()`), each one lock at a time.
- The lock is released by the CPU the task is running on at that moment, not the one
  that took it. `schedule()` unlocks `curcpu->rq.lock`.

## Switching tasks

`schedule()` holds the lock across `switch_to()`. The task that resumes releases it.

Whoever resumes calls `finish_task_switch()`, which releases the lock and then handles the
task that died in the switch (`rq->dead`): freed, or `TASK_ZOMBIE` if a parent will reap
it with `task_wait()`. New kernel threads call it from `task_start()` before enabling
interrupts. New user tasks call it from `task_start_user()`, and `iretq` then restores
interrupts from the trapframe.

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

The rq lock is taken before the buddy and slab locks, never after. `task_free()` calls
both allocators, but only after `finish_task_switch()` has released the rq lock.
