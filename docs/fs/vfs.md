# VFS

One root, a ramfs, filled at boot from the initrd (cpio newc, a Limine module) by
`initramfs_unpack()`. No mounts, no dentry cache, no cwd: paths are absolute.

- `struct inode`: mode, size, ops and a `struct mutex`. Refcounted, `iget()` / `iput()`.
- `struct file`: an inode reference and the open flags. The caller of `kernel_read()` and
  `kernel_write()` keeps the position.
- `inode_ops` are called with that inode's mutex held and no other inode's. They take kernel
  buffers only.

## Locking

These rules keep deadlocks out by construction. Break one and a lock order appears that
some later path will invert.

1. No VFS lock is held while touching user memory. Data goes through a kernel buffer:
   the inode mutex is dropped before `mm_copy_to()` takes `mm->lock`. A page fault on a file
   mapping will take `mm->lock` and then an inode's mutex, so the other order must never
   exist. The ELF loader reads a page at a time into a bounce page for this reason.
2. One inode mutex at a time. A path walk locks a directory, takes a reference on the
   child, unlocks, and only then steps into the child. `lookup` and `create` of one name
   run under the parent's mutex, so `O_CREAT | O_EXCL` holds. Operations that need two
   directories (rename, link) are not there yet. They will need a global rename mutex first.
3. The last `iput()` runs with no inode mutex held: `evict` may drop references on other
   inodes.

Lock order: inode mutex, then the buddy and slab locks. `mm->lock` never nests with an
inode mutex in either direction yet.

## exec

`task_create_exec()` loads a static ELF: `ET_EXEC` where it was linked, `ET_DYN` (static-pie)
at `0x555555554000`. `PT_INTERP` is refused until `ld.so` can be loaded next to it. Each
`PT_LOAD` becomes a vma with its `p_flags`, and its file part is copied in. Pages come
zeroed, so bss needs nothing.

The stack follows the SysV ABI, what glibc's `_start` expects: `argc`, `argv`, `envp` and
`auxv` at a 16 aligned `rsp`, the strings above them. `auxv` has what static glibc reads at
startup: `AT_PHDR` (for TLS), `AT_PAGESZ`, `AT_RANDOM` (16 bytes, not random yet),
`AT_ENTRY`, `AT_EXECFN`, `AT_PLATFORM`, `AT_HWCAP` and the ids. `rdx` is 0: no `rtld_fini`.

## Implementation

- fs/vfs.c:       inodes, files, path walk, open, mkdir, kernel_read, kernel_write
- fs/ramfs.c:     the root fs
- fs/initramfs.c: cpio newc unpacking
- fs/exec.c:      the ELF loader
- user/:          programs packed into the initrd by `make initrd`
