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

Whoever resumes calls `finish_task_switch()`, which releases the lock and then hands the
task that died in the switch (`rq->dead`) to `task_dead()`. New kernel threads call it from `task_start()` before enabling
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

## Mutexes

`struct mutex` sleeps on a `wait_queue` instead of spinning. Task context only: `mutex_lock()`
calls `might_sleep()` before it tries, so a caller holding a spinlock or with irq off panics
even when the mutex is free. Taking a mutex the task already holds, or unlocking one another
task holds, panics.

`mutex_unlock()` clears the owner before `wake_up()` walks the queue. A waiter that saw the
mutex taken is already queued, so it is woken.

## Parents and children

`tasklist_lock` protects `parent`, `children` and `sibling`, and the change to `TASK_ZOMBIE`.

- `task_dead()` runs after the rq lock is dropped. With a parent, it makes the task a zombie
  and wakes `parent->wait_chldexit`, both under `tasklist_lock`. Without one, it frees the task.
- `task_exit()` calls `exit_notify()` first: every child loses its parent. Zombies among them
  are freed there, after the lock. The others free themselves in `task_dead()`.
- `task_wait()` sleeps on its own `wait_chldexit` and looks for a zombie under
  `tasklist_lock`. The zombie leaves the list under the lock, and is freed after it.

A parent cannot be freed while a child reads `child->parent`: the parent clears it in
`exit_notify()` under the same lock, before it dies.

## Processes and threads

A process is a `struct tgroup`: its threads share mm, files and handlers, and `getpid()` is the
leader's id. `tasklist_lock` covers the group's thread list, its count of live threads, and
`exiting`.

- A thread (`clone(CLONE_THREAD)`) has no parent and frees itself in `task_dead()`.
- `live` drops in `task_dead()`, once the thread is off its cpu. At 0 the process is done:
  the leader orphans its children, then becomes a zombie for its parent, or is freed. A leader
  that exits first just waits, dead, for that.
- The children of a process hang on its leader, whichever thread forked them, and any thread
  waits on the leader's `wait_chldexit`.
- `task_group_exit()` (`exit_group()`, a fatal signal) marks the group exiting with its code
  and sends `SIGKILL` to the other threads, under `tasklist_lock`. That code becomes the
  process's status.
- A process has a group id and a session id (`pgid`, `sid`, in the `tgroup`, under `tasklist_lock`).
  A fork inherits both, the first process is its own. `setpgid()` is for the caller and its
  children, into a group of the session or a new one named by the process's own id; `setsid()` is
  refused for a group's leader. `kill(0)`, `kill(-pgid)`, `wait4(0)` and `wait4(-pgid)` work on
  them, and `task_signal_pgrp()` walks the pid hash for the leaders in a group.
- Each thread drops its mm on the way out (`exit_mm()`): off its cr3 first, then the put. A
  zombie holds no address space.

## Lock-free reads

`select_cpu()` and `balance()` read other runqueues' `nr_running` without a lock. These
are hints. `balance()` locks the busiest rq with `trylock` and checks the counts again
before moving a task.

## Lock order

`tasklist_lock`, then `sighand->lock`, then `wait_queue` lock, then rq lock, then the buddy and
slab locks. `timer_lock` and a futex bucket's lock come before a `wait_queue` lock: both wake
sleepers. A futex bucket's lock also comes before the pmap lock (`mm_read_nofault()`). Never the other way: no code takes a `wait_queue` lock while holding an rq lock. `finish_task_switch()` releases the rq
lock before it does anything else. `task_free()` calls both allocators, but only after that.

`mm->lock` and the pmap lock are not part of this chain. See [docs/mm/mm.md](../mm/mm.md).
A `mutex` may sleep, so it is never taken under any of these locks.
