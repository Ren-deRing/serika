# Runqueue Locking

Each CPU has one runqueue, `curcpu->rq` (`cpus[id].rq`). `rq->lock` protects its state:
`tasks`, `curr`, `dead`, `min_vruntime`, `load`, `nr_running`, and `need_resched`.
It also protects each task's `on_rq`, and `cpu` changes only under the lock of the rq the
task leaves.

`rq->online` is set once in `sched_init()` and is read without the lock.

## Rules

- Taking `rq->lock` from a context that can take an interrupt requires `spin_lock_irqsave()`.
  The tick handler is the exception: interrupts are already off there.
- Hold at most one rq lock at a time, with one exception: `balance()` may `spin_trylock()`
  a remote rq while holding its own. A failed trylock skips the migration. Never block on
  a second rq lock.
- Remote rq locks are taken only by `sched_add_task_cpu()`, `wake_up_process()` and the
  read helpers (`sched_nr_running()`, `sched_task_sleeping()`), each one lock at a time.
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
- The tick handler and `resched_handler()` call `preempt_schedule()`, which does its own
  irq save and restore.
- `sched_add_task_cpu()` locks the target rq with `spin_lock_irqsave()`. It sends
  `RESCHED_VECTOR` after unlocking, and only if the target is another CPU that is idle.

## Sleeping and waking

A sleeper registers on a `wait_queue` and sets `TASK_SLEEPING` inside the queue's lock, then
checks its condition, then calls `schedule()` (`wait_event()`). A waker sets the condition
and calls `wake_up()`, which walks the queue under the same lock. If the waker sees the
sleeper in the queue, it also sees `TASK_SLEEPING`. If it does not, the sleeper checks the
condition afterwards and finds it true. No wakeup is lost.

`wake_up_process()` locks the task's rq with `task_rq_lock()`: lock the rq `task->cpu` names,
recheck `task->cpu`, retry if the task moved in between.

- `on_rq`: the task is still curr, or was preempted before it slept. Only the state changes,
  to `TASK_RUNNING` (curr) or `TASK_RUNNABLE`. `schedule()` then keeps it.
- not `on_rq`: it slept through `switch_to()`, and this rq lock was released only after
  that, so its context is saved. It goes back into the tree.

`schedule()` takes a `TASK_SLEEPING` task off its rq. `preempt_schedule()` does not: the
task has not checked its condition yet, and taking it off could sleep forever on a
condition that is already true.

Per-CPU fields are read with `this_cpu_read()`, `this_cpu_write()` and `this_cpu_add()`
(`serika/percpu.h`): one `%gs` instruction each. `current_task()` is `this_cpu_read(rq.curr)`.
`curcpu->rq.curr` is two loads, and a preemption plus migration between them returns another
task. Use `curcpu->` only with interrupts off.

Sleeping is for task context only: no spinlock held, not from an interrupt handler.

## Checks

`preempt_count` is per task: `__schedule()` saves the cpu's count in the task before
`switch_to()` and loads the next task's, because a task may resume on another cpu. Low 16
bits: spinlocks held. High 16 bits: hardirq nesting, counted for vectors 32 and up only
(exceptions and NMI may arrive where `%gs` is not the kernel's yet). A new task starts with 1:
the rq lock of whoever switched to it, dropped in `finish_task_switch()`.

- `__schedule()` panics unless the count is 0. A tick or resched IPI preempting from
  interrupt context may have exactly one hardirq level and nothing else.
- `might_sleep()` panics in interrupt context, with a spinlock held, or with irq off.
  `wait_event()` calls it first, so a bad caller is found even when the condition is true.
- `spin_lock()` panics if the cpu already owns the lock. `spin_unlock()` panics if another
  cpu owns it, or if no lock is counted. A zeroed lock is free (`owner == 0`).
- The test runner compares `preempt_count` and the irq flag before and after every test.

System calls run with irq on after `syscall_entry`, like any task, and turn it off again
before `sysret`.

## Lock-free reads

`select_cpu()` and `balance()` read other runqueues' `nr_running` without a lock. These
are hints. `balance()` locks the busiest rq with `trylock` and checks the counts again
before moving a task.

## Lock order

`wait_queue` lock, then rq lock, then the buddy and slab locks. Never the other way: no code
takes a `wait_queue` lock while holding an rq lock. `finish_task_switch()` releases the rq
lock before it does anything else. `task_free()` calls both allocators, but only after that.

`mm->lock`, then the pmap lock, then the buddy and slab locks. A page fault takes `mm->lock`
with irq on and nothing else held, like a system call.
