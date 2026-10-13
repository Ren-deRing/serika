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
   directories (rename) take `rename_mutex` first, and still lock one inode at a time (see Removing and
   moving).
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

## Symlinks

A symlink is an inode (`S_IFLNK`) with its target as a string (`readlink`). `walk_parent()` works on
a copy of the path in a `PATH_MAX` buffer: where a component is a link, the buffer becomes the target
followed by what was left, and the walk goes on from the root (an absolute target) or from the dir the
link was in (a relative one). `vfs_open()` follows a link at the end the same way, 40 in all, then
`ELOOP`. `O_NOFOLLOW` stops at it: `ELOOP`, or with `O_PATH` the link itself, which `lstat` and
`readlink` use and which nothing can be read from or written to. `symlink` is an inode op like
`create`, under the dir's lock, and so is `link` (a name for an inode that exists: `/dev`). The initrd
carries links as cpio entries of type `S_IFLNK`, the target as data.

## Removing and moving

`unlink`, `rmdir` and `rename` look up the name with the dir's lock, let go, and ask the dir to take
the entry out: `ops->unlink(dir, name, len, inode)` does it only if the name is still that inode's, and
the entry's reference becomes the caller's to drop (`iput()` never runs under a lock). The inode's link
count drops under its own lock, after. A file that is open stays readable until the last close.

`rmdir` and `rename` hold `rename_mutex`, so no tree is changed under another, and then it is enough
to take one inode lock at a time. `rmdir` retires the dir first (`dir_retire`: under its own lock it must be
empty, and `nlink` becomes 0, after which `create`, `link` and `symlink` give `ENOENT`), then takes the
name out of the parent. `rename` takes out the new name (a dir over a dir only if it is empty), links the
inode under it, takes out the old name. A dir moving into itself or below is `EINVAL`, found by walking `..`
up from the target. A dir's `..` is a counted reference to its parent, changed by `link` when it moves.
`link` of a dir is `EPERM`.

## Mounts and the root

A dir with a file system on it has the root of that file system in `mounted` (a lookup of the dir gives that), and the root
has `mnt_parent`, the dir its ".." goes to. `mount()` and `umount2()` (ext4 only) keep a table of them (`/proc/mounts`).

`pivot_root(new_root, put_old)` makes `new_root`, the root of a mounted file system, the root (`vfs_root`), and the old
root is mounted on `put_old`, a dir of the new one (not a mount, below `new_root`; `EINVAL` for the others, `EBUSY` for
`put_old` being `new_root`). What was mounted in the old root stays there, so it is under `put_old` now. The references
change hands (the new root's, which its mount point had, is `vfs_root`'s; the old root's is `put_old`'s), the mount table is
edited (the first line of `/proc/mounts` is the new root, the old one is listed under `put_old`, the other targets get the
new names), and the new root's ".." is itself. Paths are strings here (a cwd is one, so is the path a file keeps for
`openat`), so every task sees the new root from then on, as if all had done `pivot_root` and `chroot(".")`. Not carried over
(there is no `MS_MOVE` or bind mount yet): `/dev` and `/proc` stay in the old root.

TODO: linux keeps a `struct path` (mount and dentry) for a task's root and cwd and for each open file, and `pivot_root` only moves the
tasks whose root or cwd was the old root; the strings here make it global, and an `openat` on a dir that was opened before the pivot
looks its path up again in the new root. Keeping (mount, inode) in the cwd and the file, and a root for each task, is the fix.

## Times

An inode has `atime`, `mtime` and `ctime` in nanoseconds since 1970, under its lock. Writes, truncates and
changes to a dir's entries set `mtime` and `ctime` (`inode_touch()`), `chmod` and `link` set `ctime`,
`utimensat` sets them as asked (`UTIME_NOW`, `UTIME_OMIT`). Reads do not touch `atime`. `CLOCK_REALTIME`
is the tsc plus the epoch read from the CMOS clock at boot (`time_init()`), which qemu sets to the host's
UTC; there is no timezone, no leap seconds, and nothing keeps it in step with the host after boot.

`/dev` is made in `vfs_init()`: the tty (`console`, `tty`, `ttyS0`) and `null`, `zero`, `full`, `random`,
`urandom` from `drivers/builtin/mem/`. `inode->rdev` is what `stat` shows.

## Pipes

A pipe is an inode with `stream` set in its ops: `stream_read` and `stream_write` take the file and run
without the inode mutex, because they sleep, and `pipe` has its own spinlock for the ring (64 KiB). The lock
is dropped before every `wake_up()` and before sleeping. Each end is a file of its own; the
last close of an end (`release`) wakes the other side: EOF for readers, `EPIPE` and `SIGPIPE`
for writers. A write of `PIPE_BUF` or less goes in whole. Sleeps are interruptible
(`-ERESTARTSYS`). With `O_NONBLOCK` on the file (open, `fcntl`, `pipe2`) a read or write that would wait gives
`-EAGAIN`, or what was done so far; the tty does the same.

A named pipe (`mknod`/`mkfifo`) is the same inode type with no one on either end yet, linked into a dir.
Opening it (`ops->open`, not for `O_PATH`) counts an end, and a read-only open waits for a writer, a
write-only one for a reader (`ENXIO` without one if `O_NONBLOCK`); `O_RDWR` never waits. `release` counts the
ends down, for both of an `O_RDWR` file.

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

A file that starts with `#!` is a script: `exec_script()` reads the first line (256 bytes at most), takes the
interpreter and the one argument after it (the whole rest of the line), and loads the interpreter with `interp
[arg] script-path args...`, as linux does, up to 4 deep (`ELOOP`). The script's path is absolute here, where
linux passes it as given. busybox's ash can not run a script without this: it would have to re-run itself
through `/proc/self/exe`.

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

## poll

An inode's `poll(inode, fflags, pt)` returns the `POLL*` bits that hold now, and with a `pt` calls
`poll_wait(pt, wq)` for the queue whose `wake_up()` can change them (at most two). With no
`poll` op a file is always ready. `poll_core()` registers the task on its own queue (the
timeout's) and every fd's, which leaves it `TASK_SLEEPING`, looks at every fd, then looks again
without registering before it calls `schedule()`: what became ready while the first pass was still
registering is seen by the second, what does after wakes it. `select` and `pselect6` turn
their sets into pollfds and back, `ppoll` and `pselect6` take a signal mask
([docs/signal.md](../signal.md)). A poll is never restarted: `-EINTR`.

`epoll`, `eventfd`, `timerfd`, `signalfd` and `flock` are in [events.md](events.md).

## Random

`get_random_bytes()` is ChaCha20 with fast key erasure, seeded at boot from `RDSEED` (or `RDRAND`)
and the tsc. With neither it says so and is not secure. `getrandom()` and `AT_RANDOM` use it,
and never wait.

## Mesa

`user/mesa.sh` copies the host's mesa (`libEGL`, `libGLESv2`, glvnd, `libgallium` with llvmpipe, `libLLVM` and what
they load) to the same paths in the initrd, and `/bin/gltest` draws into a texture on a context with no display
(`EGL_PLATFORM=surfaceless`, `LIBGL_ALWAYS_SOFTWARE=1`) and checks the pixels. It is 260 MB: it wants `-m 2G` or more.
What it asked of the kernel beyond the rest: `memfd_create` and `MAP_SHARED` of it, `prctl(PR_SET_NAME)` for its
rasterizer threads, `setpriority` and `sched_setscheduler`, `madvise(MADV_GUARD_INSTALL)` for glibc's thread stacks,
`sched_getaffinity`.

## Framebuffer

`/dev/fb0` (29:0) is the framebuffer the bootloader set up, made in `vfs_init()` by `fb_devices_create()`
(`drivers/builtin/fb/`): 32 bits per pixel, the mode the boot gave and where its colors are
(`FBIOGET_VSCREENINFO`, `FBIOGET_FSCREENINFO`; the mode can not be changed). A program reads, writes, or
`mmap`s it with `MAP_SHARED`: its `getpage` hands out the device's frames, which `frame_init()` gave a reference
that no mapping can drop, and `map_wc` has them mapped write-combining. `/bin/fbdemo [seconds]` draws a turning
triangle with llvmpipe and copies each frame to it, printing the frame rate.

