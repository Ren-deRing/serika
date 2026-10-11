# Memory Management

## Locks

`mm->lock` protects the vma tree, `brk`, `mmap_next`, and the pmap while a fault fills it.
Its order is `mm->lock`, then the pmap lock, then the buddy and slab locks. A page fault takes
`mm->lock` with irq on and nothing else held, like a system call.

`mm->map_mutex` is held across any change to the vmas (`mm_map()`, `mm_unmap()`,
`mm_protect()`, `mm_mmap()`, `mm_brk()`), because a change may shoot down, which sleeps.
It comes before `mm->lock`. Faults and user copies take `mm->lock` only.

Threads share one `mm` (`task_create_thread()`, `mm_get()`), and the last `task_free()` drops
it. Two threads faulting on the same page serialize on `mm->lock`. The second finds it mapped
and returns.

System calls touch user memory with `copy_to_user()` and `copy_from_user()`: under
`mm->lock`, against the vma's prot, filling pages that are not there yet. No page fault is
taken in the kernel.

## TLB shootdown

`mprotect()` can take rights away, so other cpus must drop what they cached. Each pmap has a
mask of the cpus with it in `cr3`. `pmap_switch()` updates it in `__schedule()`, before
`switch_to()`: the bit is set before the new `cr3` is loaded, so a shootdown that misses a cpu
wrote its ptes before that cpu walks them. It is cleared before the old `cr3` is left, and
that load flushes anyway.

`pmap_shootdown()` sends `TLB_VECTOR` to every cpu in the mask, itself included, and waits
for each to flush. One shootdown runs at a time, under a mutex. The caller holds no spinlock
and has irq on (`might_sleep()`), so two cpus shooting down at once still take each other's
ipi. A cpu that does not answer within a second panics the initiator: something kept irq
off that long. `mm_protect()` calls it after dropping `mm->lock`.

`pmap_protect()` changes the ptes only and reports whether any changed. It never flushes.

## munmap

`mm_unmap()` takes the vmas out first, under `mm->lock`. From then on a fault in the range
fails, and `map_mutex` keeps new vmas out of it. Then, a batch of 512 pages at a time: clear
the ptes (`pmap_clear()`), shoot down, and only then free the frames. A cpu that still had an
entry could otherwise write into a frame someone else got. Page tables are not freed: another
cpu may be walking them until the shootdown, so they wait for `mm_put()`.

## Related

- [docs/sched/locking.md](../sched/locking.md): the rq and wait_queue chain
- [docs/fs/vfs.md](../fs/vfs.md): the inode mutex, which user copies never hold
