# VFS

One root, a ramfs, filled at boot from the initrd (cpio newc, a Limine module) by
`initramfs_unpack()`. No mounts, no dentry cache: the kernel walks absolute paths, and
`path_from_user()` puts the cwd in front of a relative one.

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

## File descriptors

A task's `struct files` holds its fds, shared by its threads. Its spinlock covers the table
only: `fd_get()` takes a reference on the file under it and lets go, and `close` drops the
last reference after it. A new task gets the console on 0, 1 and 2.

`file->pos_lock` is held across a whole `read` or `write`, user copies included, as Linux
does. That is the one exception to rule 1: the data itself still goes through a bounce page,
so no inode mutex is held while user memory is touched, and nothing on a fault path takes a
`pos_lock`. Order: `pos_lock`, then the inode mutex, or `pos_lock`, then `mm->lock`.

The cwd is an absolute, normalized string in `struct files` (refcounted, swapped whole by
`chdir`), so threads share it and fork copies it, as with `CLONE_FS`. There are no links, so
`..` is lexical. Directory fds are not a start for a path yet. `umask` rides along.

The console is the uart's tty, a char device: see [docs/tty.md](../tty.md). `/dev/console`,
`/dev/tty` and `/dev/ttyS0` are three names for its one inode, linked by `vfs_init()`.

## Pipes

A pipe is an inode with `stream` set in its ops: `read` and `write` run without the inode
mutex, because they sleep, and `pipe` has its own spinlock for the ring (64 KiB). The lock
is dropped before every `wake_up()` and before sleeping. Each end is a file of its own; the
last close of an end (`release`) wakes the other side: EOF for readers, `EPIPE` and `SIGPIPE`
for writers. A write of `PIPE_BUF` or less goes in whole. Sleeps are interruptible
(`-ERESTARTSYS`). `O_NONBLOCK` is refused.

`read_to_user()` stops after one chunk on a stream, and `lseek`/`pread` give `ESPIPE`. A stream
has no position, so `read` and `write` skip `pos_lock`: a reader asleep on a file that fork
shared must not hold up a writer of it.

## exec

`exec_load()`, for `task_create_exec()` and `execve`, loads an ELF: `ET_EXEC` where it was
linked, `ET_DYN` (a pie) at `0x555555554000`. With `PT_INTERP`, the interpreter (`ld.so`) is loaded too, in an mmap area
of its size, and the task starts at its entry. `AT_BASE` says where it is, `AT_ENTRY` and
`AT_PHDR` still describe the program. `ld.so` maps the libraries itself, through file mmap.
Each `PT_LOAD` becomes a vma with its `p_flags`, and its file part is copied in. Pages come
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
- fs/file.c:      fd tables and the file system calls
- fs/console.c:   the console
- user/:          programs packed into the initrd by `make initrd`
