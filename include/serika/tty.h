#pragma once

#include <serika/lock.h>
#include <serika/wait.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* linux's values: this is the abi termios.h gives user space */
#define NCCS 19

#define VINTR    0
#define VQUIT    1
#define VERASE   2
#define VKILL    3
#define VEOF     4
#define VTIME    5
#define VMIN     6
#define VSTART   8
#define VSTOP    9
#define VSUSP    10
#define VEOL     11
#define VREPRINT 12
#define VDISCARD 13
#define VWERASE  14
#define VLNEXT   15

#define ISTRIP  0000040
#define INLCR   0000100
#define IGNCR   0000200
#define ICRNL   0000400

#define OPOST   0000001
#define ONLCR   0000004

#define ISIG    0000001
#define ICANON  0000002
#define ECHO    0000010
#define ECHOE   0000020
#define ECHOK   0000040
#define ECHONL  0000100
#define NOFLSH  0000200
#define ECHOCTL 0001000
#define ECHOKE  0004000
#define IEXTEN  0100000

/* the kernel's termios, and termios2 with the speeds */
struct ktermios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[NCCS];
    uint32_t c_ispeed;
    uint32_t c_ospeed;
};

#define TTY_TERMIOS_SIZE 36 /* termios is termios2 without the speeds */

struct winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

#define N_TTY_BUF_SIZE 4096 /* a power of 2 */
#define TTY_FLIP_SIZE  64

struct tty_struct;
struct inode;

/* what the device behind a tty does. no locks are held */
struct tty_operations {
    long (*write)(struct tty_struct *tty, const char *buf, size_t n);
};

/* the line discipline between the device and read(): n_tty is the only one.
 * all but read and receive_buf are called with tty->lock held */
struct tty_ldisc_ops {
    long (*read)(struct tty_struct *tty, char *buf, size_t n, bool nonblock); /* -EAGAIN if it would wait */
    void (*receive_buf)(struct tty_struct *tty, const uint8_t *cp, size_t n); /* irq context */
    void (*set_termios)(struct tty_struct *tty, const struct ktermios *old);
    void (*flush_buffer)(struct tty_struct *tty);
    long (*input_pending)(struct tty_struct *tty);
    bool (*readable)(struct tty_struct *tty); /* would read() not wait */
};

struct tty_struct {
    const struct tty_operations *ops;
    const struct tty_ldisc_ops  *ldisc;
    void                        *driver_data;

    spinlock_t        lock; /* termios, pgrp and the ldisc's data. taken from the irq: irqsave */
    struct ktermios   termios;
    struct winsize    winsize;
    long              pgrp; /* foreground, as a process id. 0: none */

    struct wait_queue read_wait;

    /* a driver fills these from its irq, and pushes */
    uint8_t           flip[TTY_FLIP_SIZE];
    size_t            flip_len;

    /* n_tty. counters, indexed modulo the size */
    uint8_t          *read_buf; /* N_TTY_BUF_SIZE */
    uint32_t          read_head;   /* next written */
    uint32_t          read_tail;   /* next read */
    uint32_t          canon_head;  /* read() may take up to here. the head when not canonical */
    bool              eof_pending; /* ^D on an empty line: the next read gives 0 */

    struct inode     *inode;
};

extern const struct tty_ldisc_ops n_tty_ops;
extern struct tty_struct *console_tty;

struct tty_struct *tty_alloc(const struct tty_operations *ops, void *driver_data);

/* a driver's irq: put characters in, then push them to the line discipline */
void tty_insert_flip_char(struct tty_struct *tty, uint8_t c);
void tty_flip_buffer_push(struct tty_struct *tty);

/* the device the tty is: /dev/console, /dev/tty and /dev/ttyS0 are this inode */
struct inode *tty_inode(struct tty_struct *tty);
