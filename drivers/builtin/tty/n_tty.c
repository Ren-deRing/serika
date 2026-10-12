#include <serika/errno.h>
#include <serika/preempt.h>
#include <serika/sched.h>
#include <serika/signal.h>
#include <serika/task.h>
#include <serika/tty.h>

#define BUF_MASK (N_TTY_BUF_SIZE - 1)

#define I_ISTRIP(t)  ((t)->termios.c_iflag & ISTRIP)
#define I_INLCR(t)   ((t)->termios.c_iflag & INLCR)
#define I_IGNCR(t)   ((t)->termios.c_iflag & IGNCR)
#define I_ICRNL(t)   ((t)->termios.c_iflag & ICRNL)
#define L_ISIG(t)    ((t)->termios.c_lflag & ISIG)
#define L_ICANON(t)  ((t)->termios.c_lflag & ICANON)
#define L_ECHO(t)    ((t)->termios.c_lflag & ECHO)
#define L_ECHOE(t)   ((t)->termios.c_lflag & ECHOE)
#define L_ECHOK(t)   ((t)->termios.c_lflag & ECHOK)
#define L_ECHONL(t)  ((t)->termios.c_lflag & ECHONL)
#define L_ECHOCTL(t) ((t)->termios.c_lflag & ECHOCTL)
#define L_NOFLSH(t)  ((t)->termios.c_lflag & NOFLSH)

/* a control char whose cc entry is 0 is off */
#define IS_CC(t, c, i) ((t)->termios.c_cc[i] && (c) == (t)->termios.c_cc[i])

static uint8_t *slot(struct tty_struct *tty, uint32_t counter) {
    return &tty->read_buf[counter & BUF_MASK];
}

static bool is_ctl(struct tty_struct *tty, uint8_t c) {
    return L_ECHOCTL(tty) && ((c < 0x20 && c != '\n' && c != '\t') || c == 0x7f);
}

/* tty->lock held, as in everything below up to receive_buf */
static void echo_char(struct tty_struct *tty, uint8_t c) {
    char s[2] = { c, 0 };

    if (is_ctl(tty, c)) {
        s[0] = '^';
        s[1] = c == 0x7f ? '?' : c ^ 0x40;
        tty->ops->write(tty, s, 2);
    } else {
        tty->ops->write(tty, s, 1);
    }
}

/* wipes an echoed char off the screen */
static void echo_erase(struct tty_struct *tty, uint8_t erased) {
    if (!L_ECHO(tty)) return;

    if (!L_ECHOE(tty)) {
        echo_char(tty, tty->termios.c_cc[VERASE]);
        return;
    }

    for (int i = is_ctl(tty, erased) ? 2 : 1; i > 0; i--) tty->ops->write(tty, "\b \b", 3);
}

static void flush_input(struct tty_struct *tty) {
    tty->read_tail = tty->read_head = tty->canon_head;
    tty->eof_pending = false;
}

static void put_char(struct tty_struct *tty, uint8_t c, uint32_t reserve) {
    if (tty->read_head - tty->read_tail >= N_TTY_BUF_SIZE - reserve) return; /* full: dropped */

    *slot(tty, tty->read_head++) = c;
}

/* one character in. signals wanted go in *sigs, sent once the lock is down */
static void receive_char(struct tty_struct *tty, uint8_t c, uint64_t *sigs) {
    if (I_ISTRIP(tty)) c &= 0x7f;

    if (c == '\r') {
        if (I_IGNCR(tty)) return;
        if (I_ICRNL(tty)) c = '\n';
    } else if (c == '\n' && I_INLCR(tty)) {
        c = '\r';
    }

    if (L_ISIG(tty)) {
        int sig = IS_CC(tty, c, VINTR) ? SIGINT : IS_CC(tty, c, VQUIT) ? SIGQUIT : 0;

        if (sig) {
            if (!L_NOFLSH(tty)) flush_input(tty);
            if (L_ECHO(tty)) echo_char(tty, c);

            *sigs |= sigmask(sig);
            return;
        }
    }

    if (L_ICANON(tty)) {
        if (IS_CC(tty, c, VERASE)) {
            if (tty->read_head != tty->canon_head) echo_erase(tty, *slot(tty, --tty->read_head));
            return;
        }

        if (IS_CC(tty, c, VKILL)) {
            while (tty->read_head != tty->canon_head) echo_erase(tty, *slot(tty, --tty->read_head));
            if (L_ECHO(tty) && L_ECHOK(tty)) tty->ops->write(tty, "\n", 1);
            return;
        }

        /* the line goes out as it is, without the ^D. on an empty one, read() gives 0 */
        if (IS_CC(tty, c, VEOF)) {
            if (tty->read_head == tty->canon_head) tty->eof_pending = true;
            else                                   tty->canon_head = tty->read_head;
            return;
        }

        if (c == '\n') {
            if (L_ECHO(tty) || L_ECHONL(tty)) tty->ops->write(tty, "\n", 1);

            put_char(tty, c, 0);
            tty->canon_head = tty->read_head;
            return;
        }
    }

    if (L_ECHO(tty)) echo_char(tty, c);

    put_char(tty, c, 1); /* room is kept for the newline */
    if (!L_ICANON(tty)) tty->canon_head = tty->read_head;
}

static void n_tty_receive_buf(struct tty_struct *tty, const uint8_t *cp, size_t n) {
    uint64_t sigs = 0;

    uint64_t flags = spin_lock_irqsave(&tty->lock);

    for (size_t i = 0; i < n; i++) receive_char(tty, cp[i], &sigs);

    long pgrp = tty->pgrp;

    spin_unlock_irqrestore(&tty->lock, flags);

    wake_up(&tty->read_wait);

    for (int sig = 1; sig <= NSIG && pgrp; sig++) {
        struct ksiginfo info = { .signo = sig, .code = SI_KERNEL };

        if (sigs & sigmask(sig)) task_signal_pgrp(pgrp, sig, &info);
    }
}

/* what read() of n may take now. a line in canonical mode, VMIN characters otherwise */
static bool readable(struct tty_struct *tty, size_t n) {
    uint32_t avail = tty->canon_head - tty->read_tail;

    if (L_ICANON(tty)) return avail || tty->eof_pending;

    size_t min = tty->termios.c_cc[VMIN];
    return avail >= (min < n ? min : n);
}

static long n_tty_read(struct tty_struct *tty, char *buf, size_t n, bool nonblock) {
    if (!n) return 0;

    uint64_t flags = spin_lock_irqsave(&tty->lock);

    /* VMIN 0: whatever is there, which may be nothing */
    while (!readable(tty, n) && !(!L_ICANON(tty) && tty->termios.c_cc[VMIN] == 0) && !tty->hung) {
        spin_unlock_irqrestore(&tty->lock, flags);

        if (nonblock) return -EAGAIN;

        int err = wait_event_interruptible(tty->read_wait, readable(tty, n) || tty->hung);
        if (err) return err;

        flags = spin_lock_irqsave(&tty->lock);
    }

    uint32_t avail = tty->canon_head - tty->read_tail;
    size_t len = avail < n ? avail : n;

    if (L_ICANON(tty)) {
        if (!avail) tty->eof_pending = false; /* the end: 0 */

        /* one line */
        for (size_t i = 0; i < len; i++) {
            if (*slot(tty, tty->read_tail + i) == '\n') {
                len = i + 1;
                break;
            }
        }
    }

    for (size_t i = 0; i < len; i++) buf[i] = *slot(tty, tty->read_tail + i);
    tty->read_tail += len;

    spin_unlock_irqrestore(&tty->lock, flags);
    return len;
}

/* canonical or not changes what is ready: what is buffered is all ready */
static void n_tty_set_termios(struct tty_struct *tty, const struct ktermios *old) {
    if ((old->c_lflag ^ tty->termios.c_lflag) & ICANON) tty->canon_head = tty->read_head;
}

static long n_tty_input_pending(struct tty_struct *tty) {
    return tty->canon_head - tty->read_tail;
}

static bool n_tty_readable(struct tty_struct *tty) {
    return L_ICANON(tty) ? readable(tty, 1) : tty->canon_head != tty->read_tail;
}

const struct tty_ldisc_ops n_tty_ops = {
    .read          = n_tty_read,
    .receive_buf   = n_tty_receive_buf,
    .set_termios   = n_tty_set_termios,
    .flush_buffer  = flush_input,
    .input_pending = n_tty_input_pending,
    .readable      = n_tty_readable,
};
