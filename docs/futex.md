# Futex

`futex()` with `WAIT`, `WAKE`, `WAIT_BITSET`, `WAKE_BITSET`, `REQUEUE` and `CMP_REQUEUE`, what
glibc's mutexes, condition variables and `pthread_join()` use. The others, PI included, are
`-ENOSYS`.

A word is keyed by (mm, address): there are no shared mappings yet, so private or not, a word
belongs to one mm. 256 buckets, each a spinlock and a list of waiters.

## Waiting

A waiter is a `futex_q` on its stack, with a `wait_queue` of its own:

1. Under the bucket lock, read the word and compare. The lock is a spinlock, so `mm->lock` (a
   mutex) cannot be taken: `mm_read_nofault()` reads the frame if the page is there. If not,
   the lock is dropped, `copy_from_user()` brings the page in, and it starts over.
2. Still under the lock, queue `q`. A waker that changed the word takes the same lock to wake,
   so it finds `q` queued: no wakeup is lost.
3. `wait_event_interruptible()` on `q.wq` until woken, timed out, or a signal (`-EINTR`).
4. Out of the list under the lock of whichever bucket `q` is in now: a requeue may have moved
   it. A waker only touches `q` with that lock held, so once the waiter has taken it, the
   stack frame can go.

A timeout is a timer (`timer_add()`) that wakes `q.wq`. `timer_del()` returns only once its
function is done, so it too is over before the frame goes.

## Exit

A thread's `clear_child_tid` (`CLONE_CHILD_CLEARTID`, `set_tid_address()`) is zeroed and woken
in `task_exit()`, before the mm is dropped: that is what `pthread_join()` sleeps on.

## Timers

One sorted list under `timer_lock`, run from cpu 0's tick (1 ms). A timer's function runs with
the lock held and must not sleep. `sleep_until()` (`nanosleep`) sleeps on a wait queue the timer
wakes, instead of yielding until the deadline.
