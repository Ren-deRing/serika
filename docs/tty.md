# TTY

Laid out as in Linux: a device driver at the bottom, a line discipline above it, and the
char device inode on top.

    uart irq -> tty_insert_flip_char() -> tty_flip_buffer_push() -> ldisc->receive_buf()
                                                                      |  (echo: ops->write)
    read(2) on /dev/tty -> inode read -> ldisc->read() <--- read_buf ring
    write(2)            -> inode write -> ops->write() -> uart

- `struct tty_operations` is what a device does: `write`. `struct tty_ldisc_ops` is the line
  discipline, `n_tty` the only one. `struct tty_struct` holds the termios, the window size,
  the foreground process and the ldisc's ring.
- The inode is a stream (`inode_ops.stream`): `read` sleeps, so no inode lock, and `read` and
  `write` skip the file's `pos_lock`. `ioctl` is an inode op as well and runs with no lock:
  its argument is user memory.
- `drivers/builtin/tty/` is the core and `n_tty`. `drivers/builtin/serial/` is the 16550 as a
  tty driver: `serial_tty_init()` makes the console tty and routes ISA irq 4 through the
  io apic (`arch/x86_64/kernel/ioapic.c`, found in the ACPI MADT) to the cpu that calls it.

## Locking

`tty->lock` covers the termios, the pgrp and the ldisc's data, and the irq takes it, so it is
always `irqsave`. Sleeping is done with it dropped: `wait_event_interruptible(read_wait)`,
and `wake_up()` is called after the unlock. Signals (`^C`) are collected under the lock and
sent after it. Echo goes out through `ops->write` under the lock: the uart is polled for
output, so it is short and it cannot sleep.

The flip buffer has one producer, the driver's irq, and needs no lock.

## n_tty

Canonical mode keeps a line in the ring until the newline: `read_head` is written,
`canon_head` is what a read may take, `read_tail` is read. A read gives one line at most.
Not canonical, `canon_head` follows `read_head` and `VMIN` characters make a read ready
(`VMIN 0`: whatever is there). `VTIME` is not there.

Handled: `ICRNL`, `INLCR`, `IGNCR`, `ISTRIP`; `ISIG` (`VINTR`, `VQUIT`, flush unless
`NOFLSH`); `ICANON` (`VERASE`, `VKILL`, `VEOF`); `ECHO`, `ECHOE`, `ECHOK`, `ECHONL`,
`ECHOCTL`. A `^D` on an empty line makes the next read return 0, on a line it ends the line
without a newline. Output is raw: `OPOST` and `ONLCR` are stored and not applied.

ioctls: `TCGETS`, `TCSETS`, `TCSETSW`, `TCSETSF` and the `2` forms, `TCFLSH`, `FIONREAD`,
`TIOCGPGRP`, `TIOCSPGRP`, `TIOCGWINSZ`, `TIOCSWINSZ`, `TIOCGSID`, `TIOCSCTTY`, `TIOCNOTTY`.
Anything else is `ENOTTY`.

## No sessions

There are no process groups, so a pgid is a process id. `^C` signals `tty->pgrp`, which
`TIOCSPGRP` sets (a job-control shell does). While it is 0 nobody gets the signal, and
`TIOCGPGRP` answers with the asker's own id.
