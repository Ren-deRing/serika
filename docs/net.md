# Sockets

Unix domain sockets only: `AF_UNIX` with `SOCK_STREAM`, `SOCK_DGRAM` and `SOCK_SEQPACKET`, and the calls `socket`,
`socketpair`, `bind`, `listen`, `connect`, `accept`, `accept4`, `shutdown`, `getsockname`, `getpeername`, `sendto`,
`recvfrom`, `sendmsg`, `recvmsg`, `sendmmsg`, `recvmmsg`, `setsockopt` and `getsockopt`. A socket is a file like a
pipe is: an `S_IFSOCK` inode with `stream` set in its ops, so `read`, `write`, `dup`, `fork`, `poll`, `FIONREAD` and
`FIONBIO` work on it. `net/socket.c` is the system calls and `net/unix.c` the sockets.

## Names

A name is a path or an abstract name (a `sun_path` that starts with a nul, every byte of it the name). Binding a path
links the socket's own inode into the file system (`vfs_link_inode()`, `EEXIST` is `EADDRINUSE`), so `stat` shows an
`S_IFSOCK`, `unlink` removes the name, and `open` of it is `ENXIO`. Connecting opens the path with `O_PATH` and takes
the socket from the inode. Abstract names are in a list (`abstract_lock`) until the socket is closed. A bind with just
the family gets a name of its own (a nul and five hex digits).

## Connections

`connect` on a stream or seqpacket socket makes the other end at once (`srv`, with the listener's credentials), joins
the two, and queues `srv` on the listener, which `accept` turns into a file. The queue holds `backlog + 1`; a full one
waits, or is `EAGAIN`. A datagram `connect` only sets where to send, and `AF_UNSPEC` clears it. Closing the last file
of a socket (`unix_release()`) ends the other side's reads and makes its writes `EPIPE`; sockets that were queued and
never accepted are closed with their listener. `shutdown` does the same for one direction.

## Data

Each send is an `skb` on the receiver's queue: its bytes, the files that go with it (`SCM_RIGHTS`, up to 253) and
the sender's credentials (and name, for datagrams). A stream reader takes bytes across skbs, but the files of a
message come with its first byte and a read stops before an skb that has some, or one from another sender. A
datagram or seqpacket read is one message, cut with `MSG_TRUNC` if the buffer is small; `MSG_PEEK` leaves it,
`MSG_WAITALL` repeats a stream read. Files are installed in the reader's table as the control data is built, with
`MSG_CMSG_CLOEXEC`; ones that do not fit are closed and `MSG_CTRUNC` says so. A queue holds `SO_RCVBUF` bytes (212992);
a message that is bigger than that goes in when the queue is empty. A stream send is cut into 64 KiB pieces, a
datagram is 128 KiB at most.

`SO_PEERCRED`, `SO_PASSCRED` (credentials in the control data), `SO_SNDBUF`/`SO_RCVBUF` (told back doubled, as linux),
`SO_RCVTIMEO`/`SO_SNDTIMEO`, `SO_TYPE`, `SO_ACCEPTCONN`, `SO_ERROR`, `SO_DOMAIN`, and the flag-like options that are
only given back as set.

## Locking

User memory is touched only with no socket lock held: `socket.c` copies the iovecs and the control data into kernel
buffers before, and out after. One socket lock at a time (a peer is reached through a reference and locked on its
own), and the files an skb carries are closed after the lock. All the waiters, `poll()` included, sleep on one queue
for all sockets and look again when it wakes: simple, and no queue can be freed under a sleeper, at the price of
waking more than it has to.

Not here: other families (`AF_INET`, `AF_NETLINK`), out of band data, `SO_LINGER`, `SCM_SECURITY`, and the cleanup
of a cycle of sockets that carry each other's files.
