# Memory Management

## Locks

`mm->lock` is a mutex. It protects the vma tree, `brk`, `mmap_next`, and the pmap while a fault
fills it, and it is held across any change to the vmas (`mm_map()`, `mm_unmap()`,
`mm_protect()`, `mm_mmap()`, `mm_brk()`), shootdowns included. Its order is `mm->lock`, then
`tlb_mutex`, then the pmap lock, then the buddy and slab locks (the slab's is a cache's lock; no slab
code holds it while it calls the buddy).

A mutex may sleep, so it is taken with irq on and no spinlock held, from task context only.
The page fault handler turns irq on before it takes it. Signal delivery does the same on the
way to user mode (`exit_to_user()`), because writing the frame goes through it. Code that
cannot sleep, such as the futex fast path under a hash bucket's spinlock, must not use
`copy_from_user()` and needs an accessor that does not take `mm->lock`.

Threads share one `mm` (`clone(CLONE_VM)`, `mm_get()`). Each drops its reference when it
exits (`exit_mm()`, after leaving its cr3), so the last thread's exit frees it. Two threads faulting on the same page serialize on `mm->lock`. The second finds it mapped
and returns. A fault waits behind a shootdown of the same mm: it sleeps with irq on, so the
ipi it is waiting for is still taken.

System calls touch user memory with `copy_to_user()` and `copy_from_user()`: under
`mm->lock`, against the vma's prot, filling pages that are not there yet. No page fault is
taken in the kernel.

## File mappings

A vma may map a file, `MAP_PRIVATE` only: it holds a reference on the `struct file` and the
offset of its first page. A fault reads the page into a fresh frame with `kernel_read()`,
zeros past the end of the file, and the frame is the mm's own from then on: nothing is
shared with the file or with other mms yet (no page cache). Splitting, trimming and forking a
vma carry the reference and the offset along (`vma_piece()`).

The read takes the inode's mutex under `mm->lock`. That is the order: `mm->lock`, then the
inode mutex. The vfs never touches user memory with an inode mutex held, so the other order
does not exist. `MAP_FIXED` unmaps whatever was in its range first, as `ld.so` expects when
it lays segments over the area it reserved.

## Copy-on-write

Each frame a user pmap maps has a count in its `struct page` (`refs`): 1 when a fault brings
it in, one more for each mm `fork()` shares it with. `munmap()`, a shrinking `brk()` and
`mm_put()` drop a reference, and the last one frees the frame.

`mm_fork()` gives the child the same vmas and maps every present page into it read-only.
The parent's writable pages turn read-only too, and the parent's pmap is shot down: another
thread of the parent may still hold a writable entry.

A write to a present read-only page in a writable vma breaks the sharing (`cow_break()`),
both from a fault and from `copy_to_user()`, which writes through the direct map:

- `refs` is 1: nobody else maps it any more. The pte gets its rights back.
- Else: a copy of the frame replaces it in this pmap, the old entry is shot down for this mm's
  other threads, and the shared frame loses a reference.

A shootdown with no other cpu in the mask flushes this cpu only, without an ipi. A
single-threaded process breaking its own copy-on-write goes that way.

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
off that long. `mm_protect()` and `mm_unmap()` call it with `mm->lock` held.

`pmap_protect()` changes the ptes only and reports whether any changed. It never flushes.

## munmap

`mm_unmap()` takes the vmas out first. From then on a fault in the range
fails, and `mm->lock` keeps new vmas out of it. Then, a batch of 512 pages at a time: clear
the ptes (`pmap_clear()`), shoot down, and only then free the frames. A cpu that still had an
entry could otherwise write into a frame someone else got. Page tables are not freed: another
cpu may be walking them until the shootdown, so they wait for `mm_put()`.

A shrinking `brk()` goes the same way: the pages past the new break are unmapped and freed,
so growing again faults in zeroed pages, which malloc counts on.

## Shared mappings

`MAP_SHARED` of a file is the file's own pages: the vma has `shared` set, and a fault asks the inode for the
page (`getpage`, called with `mm->lock` held and taking the inode lock, the order a file fault has always had).
ramfs hands out its frames with a reference taken, so a frame is the file's and each mapping's, and `ftruncate` or
the last close can not free one that is still mapped (`frame_init()`, `frame_get()`, `frame_put()`). Nothing is
copied: a write goes straight into the page, `fork` maps the same frames in the child with the same rights (a
private mapping is made read only on both sides instead), and a read only entry that a write finds in a writable
vma just gets its right back. A page past the end of the file does not map (`SIGSEGV`; linux sends `SIGBUS`).
`MAP_SHARED | MAP_ANONYMOUS` and `memfd_create()` are files with no name (`ramfs_anon_file()`). `mremap`
keeps `shared`.

## Related

- [docs/sched/locking.md](../sched/locking.md): the rq and wait_queue chain
- [docs/fs/vfs.md](../fs/vfs.md): the inode mutex, which user copies never hold

## madvise

`MADV_DONTNEED` and `MADV_FREE` call `mm_discard()`: the range must be mapped, the vmas stay,
and the pages go through the same clear, shootdown and `frame_put()` as `munmap` (`drop_pages()`).
Anonymous memory reads zero after, a private file mapping reads the file again. Hints that
change nothing (`NORMAL`, `RANDOM`, `SEQUENTIAL`, `WILLNEED`, `HUGEPAGE`, `NOHUGEPAGE`,
`DONTDUMP`, `DODUMP`, `MERGEABLE`, `UNMERGEABLE`, `COLD`, `PAGEOUT`) are taken. The ones that
change what fork or a file does (`DONTFORK`, `WIPEONFORK`, `REMOVE`) are `EINVAL`.

## Slab

`kmalloc()` is a set of caches of 8 to 4096 bytes (`mm/slab.c`), each of slabs that are one page. A cpu allocates
from its own slab of a cache, and frees to it, with irq off and nothing else: the common `kmalloc()` and `kfree()`
touch no shared memory. A slab belongs to a cpu while it is frozen: that is its active slab or one of the few on its
partial list.

A free from another cpu cannot use the owner's freelist. It pushes the object on the slab's own freelist with one
compare and swap of the slab's state, a word with the head of that freelist, the number of objects in use and the
frozen bit. The owner takes the whole freelist when it runs out. Only pushes and takes of everything happen, so
there is no ABA. Where a free finds the slab decides the rest:

- Frozen: that is all. Its cpu finds the object when it needs it.
- Not frozen and full (on no list): the free takes it for its own cpu (sets frozen with the push) and puts it on that
  cpu's partial list. A partial list longer than 4 gives the oldest back (unfrozen) to the cache.
- Not frozen, not full: it is on the cache's partial list. If the free empties it and the cache has more than 4 slabs
  (`MIN_PARTIAL`), it goes to the buddy.

The cache's lock covers its partial list and each slab's `on_list` flag, and nothing else; it is taken to refill a cpu
and to give slabs back. It is never held across `alloc_pages()` or `free_pages()`. An allocation refills from, in order,
the active slab's freelist (what other cpus freed), the cpu's partial list, the cache's partial list (the first slab is
active, up to two more go on the partial list), and the buddy.

A free checks that the object is in a slab and, when it is not on the cpu's active slab, that it is at an
object of its cache. A bad one panics.

Not here: shrinking the caches under memory pressure beyond the empty slabs, caches that are not `kmalloc`'s sizes,
redzones and poisoning, and a node for each memory node.
