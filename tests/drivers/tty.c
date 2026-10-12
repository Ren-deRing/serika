#include <serika/errno.h>
#include <serika/fcntl.h>
#include <serika/string.h>
#include <serika/tty.h>
#include <serika/vfs.h>

#include <tests/test.h>

/* as the uart's irq would */
static void feed(struct tty_struct *tty, const char *s) {
    while (*s) tty_insert_flip_char(tty, *s++);

    tty_flip_buffer_push(tty);
}

static bool reads(struct file *f, const char *want, size_t len) {
    char buf[32];
    uint64_t pos = 0;

    return kernel_read(f, buf, sizeof(buf), &pos) == (long)len && memcmp(buf, want, len) == 0;
}

/* canonical mode, raw mode, ^D and ^C, on the console tty without its echo */
bool test_tty_ldisc() {
    struct tty_struct *tty = console_tty;
    struct file *f;
    struct ktermios saved = tty->termios;
    char buf[32];
    uint64_t pos = 0;

    TEST_ASSERT(vfs_open("/dev/tty", O_RDWR, 0, &f) == 0);
    TEST_ASSERT(f->inode == console_inode);

    tty->termios.c_lflag &= ~ECHO;

    /* erase, and a CR is a NL */
    feed(tty, "ab\x7f" "c\r");
    TEST_ASSERT(reads(f, "ac\n", 3));

    /* kill */
    feed(tty, "xy\x15" "z\n");
    TEST_ASSERT(reads(f, "z\n", 2));

    /* a read takes one line */
    feed(tty, "one\ntwo\n");
    TEST_ASSERT(reads(f, "one\n", 4));
    TEST_ASSERT(reads(f, "two\n", 4));

    /* a line cut short by ^D is read as it is, and a ^D on an empty one is the end */
    feed(tty, "abc\x04");
    TEST_ASSERT(reads(f, "abc", 3));
    feed(tty, "\x04");
    TEST_ASSERT(reads(f, "", 0));

    /* a read of less leaves the rest */
    feed(tty, "hello\n");
    TEST_ASSERT(kernel_read(f, buf, 2, &pos) == 2 && memcmp(buf, "he", 2) == 0);
    TEST_ASSERT(reads(f, "llo\n", 4));

    /* ^C drops what was typed. no foreground to signal */
    feed(tty, "ab\x03" "c\n");
    TEST_ASSERT(reads(f, "c\n", 2));

    /* raw: ready as typed, and VMIN 0 reads nothing at once */
    tty->termios.c_lflag &= ~ICANON;
    tty->termios.c_cc[VMIN] = 0;
    TEST_ASSERT(reads(f, "", 0));

    tty->termios.c_cc[VMIN] = 1;
    feed(tty, "xy\n");
    TEST_ASSERT(reads(f, "xy\n", 3));

    /* typed in raw, still there in canonical */
    feed(tty, "raw");
    tty->termios.c_lflag |= ICANON;
    TEST_ASSERT(tty->ldisc->input_pending(tty) == 3);
    TEST_ASSERT(reads(f, "raw", 3));

    tty->termios = saved;
    vfs_close(f);
    return true;
}
