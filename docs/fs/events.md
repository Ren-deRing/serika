# epoll, eventfd, timerfd, signalfd, flock

What libwayland's event loop and the programs around it wait on. All of it is built on `poll` (see
[vfs.md](vfs.md)): an inode's `poll()` op says what is ready and which queue wakes the waiter. None of the
files below has a name: `anon_fd()` makes the inode (mode `0600`, no type) and a file on it in the current task,
and the inode's `evict` frees its state. Each of the first four is a stream (no inode lock, no position), so
`read` and `write` get the file and its `O_NONBLOCK`.

## epoll

`fs/epoll.c`. An `epoll` is a list of items: an fd, the `struct file` that fd was when it was added, the events
asked for and the user's `data`. `epoll_wait` does not keep waiters of its own: it copies the items that are on
into pollfds and calls `poll_core()`, the same wait `poll` has (the timeout, signals, `-EINTR`), then turns what
is ready back into events. `epoll_pwait` and `epoll_pwait2` take a signal mask like `ppoll`.

The file in an item is only compared and never held: a socket must see its last `close()` even when an epoll
lists it, as in linux. So an item can outlive its file: if its fd is closed, the next wait finds `POLLNVAL` and
drops it; if the number is another file's by then, that is seen when the item is ready (the file differs) and
`EPOLL_CTL_ADD` of the number replaces it. An item that is only stale is dropped no sooner.

- Level triggered. `EPOLLET` is taken but works as level: a reader that does not read everything is told again,
  where linux would be quiet. `EPOLLONESHOT` is exact (off until `EPOLL_CTL_MOD`); `EPOLLEXCLUSIVE` and
  `EPOLLWAKEUP` are ignored.
- `EPOLLERR` and `EPOLLHUP` are always on. `EPOLLRDNORM`, `EPOLLWRNORM` are `IN` and `OUT`; `EPOLLRDHUP` is told with
  `HUP`.
- The ones reported move to the end of the list, so a `maxevents` that is smaller than the ready ones takes
  turns.
- A file with no `poll` op (a regular file) is `EPERM`.
- An epoll is a file that `poll` and another epoll take: ready when an item in it is. An epoll that holds an
  epoll cannot be put in one (`ELOOP`): two levels and no loop. What a nested epoll waits on is registered
  as far as the poll table has room (`2 * nfds + 1` places), so one that is deeper or wider may not wake its waiter.
- A change of the items by another thread is seen at the next wake of a thread that waits: the wait is on a copy.

## eventfd

`fs/eventfd.c`. A 64-bit counter. A read takes it all (or one, with `EFD_SEMAPHORE`) and sleeps at zero; a write adds
and sleeps when it would pass `0xfffffffffffffffe`. Readers and writers share one queue.

## timerfd

`fs/timerfd.c`. `CLOCK_REALTIME`, `CLOCK_MONOTONIC` and `CLOCK_BOOTTIME`, relative or absolute
(`TFD_TIMER_ABSTIME`), once or with an interval. The expirations are counted when somebody looks (`read`,
`poll`, `gettime`): from the time of the next one and the interval. The kernel timer only wakes the sleepers; it
is set again when a look finds the next one is later. A new setting counts from zero and returns the old one.
`TFD_TIMER_CANCEL_ON_SET` is taken and does nothing: the clock is never set.

## signalfd

`fs/signalfd.c`. Reads the signals of its mask that are pending on the task that reads: signals are per task here, so
a `signalfd` shared with another thread gives that thread's. The signals have to be blocked, or they are delivered
as usual. `send_signal()` wakes the task, which looks again: the file has no queue of its own that anything
wakes. `signalfd4` with an fd changes the mask. `ssi_uid` and the rest that the kernel does not keep are zero.

## flock

`fs/flock.c`. One list of locks for all inodes (`flock_lock`): the lock belongs to the open file, not to an fd, so a
`dup` or a `fork` shares it, and it goes with the file's last reference (`flock_release()` in `vfs_close()`). A
shared lock goes against an exclusive one and an exclusive against any. A change of a lock lets go of the old one
first, as linux does, so it is not atomic and a refused `LOCK_NB` change leaves the file unlocked. Waiters sleep on one
queue and look again when any lock is let go. No deadlock check, and no `fcntl` locks.
