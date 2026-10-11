# Signals

Linux's numbers, structures and frame, so static glibc's handlers, `raise()`, `abort()` and
`siglongjmp()` work unchanged.

- `struct sighand`: the 64 actions, refcounted and shared by threads like `files`. Its lock
  also covers `pending` and `blocked` of the tasks using it.
- Each task has its own `blocked`, `pending` and one `ksiginfo` per signal. Real-time signals
  do not queue: a second one before the first is taken is the same bit.
- `kill()` finds user tasks by id in a hash under `tasklist_lock`. A task found there is not
  freed until the lock is dropped: `task_free()` takes it out under the same lock.

## Delivery

`exit_to_user()` is the one place signals are acted on: the end of every system call and of
every interrupt or exception from user mode, with irq off. It schedules while needed, then
takes one signal:

- `SIG_DFL` that terminates: the task exits with the signal number as its status. The stop
  signals are logged and ignored: no job control yet.
- A handler: the user's fpu state (`xsave`, standard form) and an `rt_sigframe` go below the
  red zone, then the handler runs with `rdi`, `rsi`, `rdx` = signal, `siginfo`, `ucontext`.
  `SA_RESTORER` is required: glibc always gives one.

A sender sets the bit, then wakes the task: a sleeper wakes, a task running on another cpu
gets a resched ipi and comes through `exit_to_user()`. Exceptions from user mode call
`force_sig()`: a handler gets them, or the task dies, even if it blocked or ignored them.

Interruptible sleeps (`wait_event_interruptible()`, `task_wait()`, `nanosleep`) return
`-ERESTARTSYS` or `-EINTR` once a signal is pending. On the way out `-ERESTARTSYS` becomes a
restart (the number is in `tf->error`, `rip` goes back 2), or `-EINTR` when a handler without
`SA_RESTART` runs. It never reaches user mode.

## rt_sigreturn

The frame is the user's: anything in it may be hostile. Before anything is restored:

- `rip` and `rsp` must be user, canonical addresses. `iretq` to anything else faults in the
  kernel.
- `rflags` takes only the arithmetic flags, TF, DF, AC and RF from the frame. IF stays on,
  IOPL, NT and VM are never taken.
- `cs` and `ss` are not read from the frame at all.
- The fpu area is copied into the kernel and checked as `xrstor` would: `xstate_bv` within
  xcr0, `xcomp_bv` and the rest of the header zero, no reserved MXCSR bit. Only then
  `xrstor`.

It returns through `iretq`: `sysret` would lose `rcx` and `r11`. A bad frame kills the task
with `SIGSEGV`.
