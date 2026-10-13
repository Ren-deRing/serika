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

# The IPv4 base

The first part of the internet side: an interface, packets, Ethernet, ARP, IPv4 and ICMP echo. There are no sockets on it
yet (UDP, TCP and `AF_INET` come after). The header is `serika/net.h`; the code is `net/` and, for the card,
`drivers/builtin/net/e1000e.c`.

## Packets and interfaces

A packet is an `nbuf`: one 2 KiB `kmalloc` object with 64 bytes of room in front, so the headers go on as the packet
goes down (`nbuf_push()`) and come off as it goes up (`nbuf_pull()`). It is not the `skb` of the unix sockets, which is
something else.

An interface is a `struct netdev`: name, MAC, MTU, flags (`UP`, `LOOPBACK`, `LINK`), address, mask and gateway, and two
calls, `xmit()` (takes the packet and frees it, sent or not) and `poll()` (empties the receive ring into `net_rx()`).
`lo` (127.0.0.1/8) is made by `netdev_init()`: it has no Ethernet, what it sends is queued and comes in again through
`netd`. The first interface that is not `lo` gets a fixed 10.0.2.15/24 with the gateway 10.0.2.2 (QEMU's user network)
when it is registered. TODO: DHCP.

## e1000e

The Intel 82574L (PCI 8086:10d3, QEMU's `-device e1000e`). It was chosen over the older e1000 because the kernel has MSI
and no routing of the PCI interrupt lines. One transmit and one receive ring of 256 legacy descriptors, a 2 KiB buffer
each; a received frame is copied out into an `nbuf`, and a frame to send is copied into its buffer (padded to 60). The
interrupt (MSI) only reads `ICR` and wakes `netd`; the link is read from `STATUS` when `netd` polls. A full transmit ring
drops the frame (`-ENOBUFS`). Not here: checksum offload, VLAN, jumbo frames, more than one queue.

## The way a packet goes

Receive: interrupt, `netd`, `poll()`, `net_rx()`, `eth_rx()`, `arp_rx()` or `ip_rx()`, `icmp_rx()`. Send: `ip_tx()` finds
the route (an address of ours or 127/8 is `lo`, one in the subnet of an interface is sent to directly, the rest to the
gateway), puts on the IP header, and `arp_tx()` finds the MAC of the next hop and `eth_tx()` puts the frame on the card.

All of the state of the stack is under one mutex, `net_lock`: `netd` holds it while it works and so does whoever sends.
A wait (`net_wait()`) lets the lock go and sleeps on `net_wq`, which `netd` wakes after each round and from its 100 ms
tick, so a wait with a deadline looks again at least that often.

## ARP and IPv4

ARP has 16 entries. A packet for an address that is not known waits in the entry (up to 3) while a request goes out, again
each second, three times; then the packets are dropped. A valid entry lives 5 minutes. A request for our address is
answered, and the sender of any packet that speaks to us is learned.

IPv4 checks the version, the header length, the total length and the checksum; takes what is for the interface's address,
the broadcasts, or anything on `lo`; there is no forwarding. A fragment is dropped, and none is sent (DF is set, the TTL
is 64). `icmp_ping()` sends an echo request and waits for the reply (one at a time); an echo request to us is answered.

# Sockets and UDP

## The kinds of sockets

`socket.h` has `struct proto_ops`, the calls of `socket(2)` (bind, connect, listen, accept, getname, shutdown, sendmsg,
recvmsg, setsockopt, getsockopt) and the file's own (read, write, poll, ioctl, release, put). Every kind of socket
starts with a `struct sock { ops }` and the inode of a socket points to it. `net/socket.c` is the system calls, which
find the file's socket and call its ops, and the unix kind (`unix_op_*`, `unix_proto_ops`); `net/inet.c` is the
`AF_INET` kind (`inet_proto_ops`). `socket(AF_INET, SOCK_DGRAM, 0 or IPPROTO_UDP)` makes a UDP socket; `SOCK_STREAM`
and `SOCK_RAW` are `ESOCKTNOSUPPORT` until TCP.

## UDP

A socket is a `struct isock`, all of it under `net_lock`. A waiter (`recv`) lets the lock go in `net_wait_intr()`, which a
signal ends (`-ERESTARTSYS`) and `SO_RCVTIMEO` limits. No lock is held across a copy to or from user memory: the
datagram is copied to a kernel buffer and out after. Bound sockets are in one list; a datagram goes to the socket that
fits best (connected before bound, an address before any). `bind` to port 0 takes a port from 49152 up;
`EADDRINUSE` for a port that overlaps one in use unless both have `SO_REUSEADDR`. A datagram is one IP packet,
1472 bytes at most (`EMSGSIZE`). The checksum (with the pseudo header) is made and checked, a datagram with a bad one is
dropped, a datagram to a port nobody has is answered by ICMP port unreachable (the error does not reach any socket:
TODO). A queue holds `SO_RCVBUF` bytes (212992), the rest is dropped. A broadcast needs `SO_BROADCAST`. Not here:
multicast, `MSG_ERRQUEUE`, `IP_PKTINFO` (the IP options that glibc's resolver sets are taken and not used).

The kernel uses UDP too (DHCP): `udp_open()`, `udp_bind_k()`, `udp_send_k()`, `udp_recv_k()`, `udp_close()` (with
`net_lock` taken inside).

## Interfaces

`SIOCGIFCONF`, `SIOCGIFNAME`, `SIOCGIFFLAGS`, `SIOCSIFFLAGS` (up and down), `SIOCGIFADDR`, `SIOCSIFADDR`,
`SIOCGIFNETMASK`, `SIOCSIFNETMASK`, `SIOCGIFBRDADDR`, `SIOCGIFMTU`, `SIOCGIFHWADDR` and `SIOCGIFINDEX` on any socket.
The list of interfaces for `getifaddrs()` and `ip` is netlink, later.

# DHCP

`net/dhcp.c` is a client for the first interface that is not `lo`, a kernel thread (`dhcpd`) started by `dhcp_init()` after
the file system is up. It uses the kernel's UDP socket on port 68 (`udp_send_k()`). The interface starts with the static
10.0.2.15/24 that it is registered with, and that stays until there is a lease (and for good if there is no server: the
client tries again each 30 s).

DISCOVER (broadcast flag set, so an offer can come to an interface that has no address), OFFER, REQUEST, ACK, up to four
tries with the wait doubled (2, 4, 8, 16 s). The ACK gives the address, mask, router (`gw`), the first DNS server (`dns`)
and the lease; `/etc/resolv.conf` is written with the DNS server. Then the lease is kept:

- at T1 (the server's option 58, or half of the lease) a REQUEST with `ciaddr` goes to the server that gave it (renewing),
  again at half of what is left to T2 (not sooner than 60 s);
- at T2 (option 59, or 7/8) it goes to anyone (rebinding), until the lease is over;
- an ACK gives a new lease (`dhcp_renewals()` counts them); a NAK or the end of the lease drops the address (the interface
  has none) and it begins again with DISCOVER.

T1 and T2 that cannot be (not T1 < T2 < lease) are replaced by the defaults, a lease of `0xffffffff` is never renewed.
`dhcp_renew_now()` makes the wait for T1 end at once; the test uses it to see a renewal go through QEMU's server. Not here:
the options for a host name, NTP, domain search; `INFORM`, `RELEASE` when the interface goes down; more than one interface.
