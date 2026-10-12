#include <serika/cpu.h>
#include <serika/errno.h>
#include <serika/fcntl.h>
#include <serika/input.h>
#include <serika/mm.h>
#include <serika/page.h>
#include <serika/panic.h>
#include <serika/poll.h>
#include <serika/preempt.h>
#include <serika/printk.h>
#include <serika/sched.h>
#include <serika/signal.h>
#include <serika/slab.h>
#include <serika/stat.h>
#include <serika/string.h>
#include <serika/task.h>
#include <serika/timer.h>
#include <serika/vfs.h>

#include <asm/arch.h>

/* /dev/input/event<n>: the events of a device, as linux gives them to libinput. each open file has a queue of its own;
 * the device's entries in /sys and the udev database (what libudev looks for) are written when it is registered */

#define EVDEV_MAJOR 13
#define EVDEV_MINOR_BASE 64

#define EVDEV_BUFFER 128 /* events a file keeps. past that they are dropped and the program told with SYN_DROPPED */
#define EVDEV_VERSION 0x010001

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

#define NSEC_PER_SEC 1000000000ULL

struct evdev_client {
    list_node            node;
    struct input_dev    *dev;
    spinlock_t           lock; /* the ring */
    struct input_event  *ring;
    unsigned             head, tail; /* counts: the next to read and the next to write */
    struct wait_queue    wq;
    int                  clock;
};

static const struct inode_ops evdev_ops;

static int next_index;

/* ---- the file ---- */

static int evdev_open_file(struct file *f) {
    struct input_dev *dev = f->inode->priv;
    struct evdev_client *c = kmalloc(sizeof(*c));

    if (!c) return -ENOMEM;

    c->ring = kmalloc(EVDEV_BUFFER * sizeof(struct input_event));

    if (!c->ring) {
        kfree(c);
        return -ENOMEM;
    }

    c->dev = dev;
    c->head = c->tail = 0;
    c->clock = CLOCK_REALTIME;
    spin_lock_init(&c->lock);
    wait_queue_init(&c->wq);

    uint64_t flags = spin_lock_irqsave(&dev->lock);
    list_add_tail(&c->node, &dev->clients);
    spin_unlock_irqrestore(&dev->lock, flags);

    f->priv = c;
    return 0;
}

static void evdev_release_file(struct file *f) {
    struct evdev_client *c = f->priv;
    uint64_t flags = spin_lock_irqsave(&c->dev->lock);

    list_del(&c->node); /* nothing writes to it after this: input_event() holds the same lock */
    spin_unlock_irqrestore(&c->dev->lock, flags);

    kfree(c->ring);
    kfree(c);
}

static uint64_t now_ns(int clock) {
    if (clock == CLOCK_REALTIME) return realtime_ns();

    uint64_t tsc = rdtsc(), freq = this_cpu_read(tsc_freq_hz);

    return tsc / freq * NSEC_PER_SEC + tsc % freq * NSEC_PER_SEC / freq;
}

/* dev lock held */
static void client_push(struct evdev_client *c, uint16_t type, uint16_t code, int32_t value) {
    struct input_event ev = { .type = type, .code = code, .value = value };
    uint64_t ns = now_ns(c->clock);

    ev.tv_sec  = ns / NSEC_PER_SEC;
    ev.tv_usec = ns % NSEC_PER_SEC / 1000;

    spin_lock(&c->lock);

    if (c->tail - c->head >= EVDEV_BUFFER) {
        /* the reader is behind: what it has is of no use, it is told to ask for the state again */
        c->head = c->tail;
        ev.type = EV_SYN;
        ev.code = SYN_DROPPED;
        ev.value = 0;
    }

    c->ring[c->tail++ % EVDEV_BUFFER] = ev;

    spin_unlock(&c->lock);
}

void input_event(struct input_dev *dev, uint16_t type, uint16_t code, int32_t value) {
    uint64_t flags = spin_lock_irqsave(&dev->lock);

    if (type == EV_KEY && code <= KEY_MAX && value != 2) { /* 2 is the autorepeat of a key that is down */
        if (value) input_set_bit(dev->key, code);
        else       dev->key[code / 8] &= ~(1u << (code % 8));
    }

    for (list_node *n = dev->clients.next; n != &dev->clients; n = n->next) {
        struct evdev_client *c = container_of(n, struct evdev_client, node);

        client_push(c, type, code, value);
        wake_up(&c->wq);
    }

    spin_unlock_irqrestore(&dev->lock, flags);
}

static long evdev_read(struct file *f, void *buf, size_t n) {
    struct evdev_client *c = f->priv;
    size_t max = n / sizeof(struct input_event), done = 0;

    if (!max) return -EINVAL;

    for (;;) {
        uint64_t flags = spin_lock_irqsave(&c->lock);

        while (c->head != c->tail && done < max) {
            ((struct input_event *)buf)[done++] = c->ring[c->head++ % EVDEV_BUFFER];
        }

        spin_unlock_irqrestore(&c->lock, flags);

        if (done) return done * sizeof(struct input_event);
        if (f->flags & O_NONBLOCK) return -EAGAIN;

        int err = wait_event_interruptible(c->wq, c->head != c->tail);
        if (err) return err;
    }
}

/* a program writes events to a device: the LEDs of a keyboard, mostly. there are none to light, so they are taken */
static long evdev_write(struct file *f, const void *buf, size_t n) {
    (void)f; (void)buf;

    if (n % sizeof(struct input_event)) return -EINVAL;
    return n;
}

static uint32_t evdev_poll(struct file *f, struct poll_table *pt) {
    struct evdev_client *c = f->priv;

    poll_wait(pt, &c->wq);

    return c->head != c->tail ? POLLIN : 0;
}

/* ---- ioctl ---- */

#define IOC_READ  2u
#define IOC_WRITE 1u

#define IOC_DIR(cmd)  ((cmd) >> 30)
#define IOC_TYPE(cmd) (((cmd) >> 8) & 0xff)
#define IOC_NR(cmd)   ((cmd) & 0xff)
#define IOC_SIZE(cmd) (((cmd) >> 16) & 0x3fff)

/* a bitmap or a string to the program: as much as its buffer takes, the rest zeros. the length it gets is the return */
static long put_bytes(unsigned long arg, size_t size, const void *data, size_t have) {
    if (size > 4096) size = 4096;

    uint8_t *tmp = kmalloc(size ? size : 1);
    if (!tmp) return -ENOMEM;

    memset(tmp, 0, size);
    memcpy(tmp, data, have < size ? have : size);

    long err = copy_to_user(arg, tmp, size) != 0 ? -EFAULT : (long)size;

    kfree(tmp);
    return err;
}

static const uint8_t *type_bits(struct input_dev *dev, unsigned ev, size_t *size) {
    switch (ev) {
        case EV_SYN: *size = sizeof(dev->evbit);  return dev->evbit;
        case EV_KEY: *size = sizeof(dev->keybit); return dev->keybit;
        case EV_REL: *size = sizeof(dev->relbit); return dev->relbit;
        case EV_ABS: *size = sizeof(dev->absbit); return dev->absbit;
        case EV_MSC: *size = sizeof(dev->mscbit); return dev->mscbit;
        case EV_LED: *size = sizeof(dev->ledbit); return dev->ledbit;
        case EV_SW:  *size = sizeof(dev->swbit);  return dev->swbit;
    }

    *size = 0;
    return NULL;
}

static long evdev_ioctl(struct file *f, unsigned long cmd, unsigned long arg) {
    struct evdev_client *c = f->priv;
    struct input_dev *dev = c->dev;

    if (IOC_TYPE(cmd) != 'E') return -ENOTTY;

    unsigned nr = IOC_NR(cmd);
    size_t size = IOC_SIZE(cmd);

    /* a number each */
    switch (cmd) {
        case 0x80044501: { /* EVIOCGVERSION */
            int32_t v = EVDEV_VERSION;
            return copy_to_user(arg, &v, sizeof(v));
        }

        case 0x80084502: { /* EVIOCGID */
            uint16_t id[4] = { dev->bustype, dev->vendor, dev->product, dev->version };
            return copy_to_user(arg, id, sizeof(id));
        }

        case 0x80084503: { /* EVIOCGREP: no autorepeat of ours */
            int32_t rep[2] = { 0, 0 };
            return copy_to_user(arg, rep, sizeof(rep));
        }

        case 0x40084503: /* EVIOCSREP */
        case 0x40044590: /* EVIOCGRAB: one reader at a time is the program's business here */
        case 0x40044591: /* EVIOCREVOKE */
            return 0;

        case 0x400445a0: { /* EVIOCSCLOCKID */
            int32_t clock;

            if (copy_from_user(&clock, arg, sizeof(clock)) != 0) return -EFAULT;
            if (clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC && clock != 7) return -EINVAL; /* 7: boottime */

            c->clock = clock == CLOCK_REALTIME ? CLOCK_REALTIME : CLOCK_MONOTONIC;
            return 0;
        }
    }

    if (IOC_DIR(cmd) != IOC_READ) return -ENOTTY;

    if (nr == 0x06) return put_bytes(arg, size, dev->name, strlen(dev->name) + 1);      /* EVIOCGNAME */
    if (nr == 0x07) return put_bytes(arg, size, dev->phys, strlen(dev->phys) + 1);      /* EVIOCGPHYS */
    if (nr == 0x08) return put_bytes(arg, size, "", 1);                                  /* EVIOCGUNIQ */
    if (nr == 0x09) return put_bytes(arg, size, dev->propbit, sizeof(dev->propbit));     /* EVIOCGPROP */
    if (nr == 0x0a) return -EINVAL;                                                       /* EVIOCGMTSLOTS */

    if (nr == 0x18) { /* EVIOCGKEY */
        uint8_t keys[sizeof(dev->key)];

        uint64_t fl = spin_lock_irqsave(&dev->lock);
        memcpy(keys, dev->key, sizeof(keys));
        spin_unlock_irqrestore(&dev->lock, fl);

        return put_bytes(arg, size, keys, sizeof(keys));
    }

    if (nr == 0x19 || nr == 0x1a || nr == 0x1b) return put_bytes(arg, size, "", 0); /* LED, SND, SW state: all off */

    if (nr >= 0x20 && nr < 0x40) { /* EVIOCGBIT(ev) */
        size_t have;
        const uint8_t *bits = type_bits(dev, nr - 0x20, &have);

        return put_bytes(arg, size, bits, have);
    }

    if (nr >= 0x40 && nr <= 0x40 + ABS_MAX) { /* EVIOCGABS(abs) */
        unsigned abs = nr - 0x40;

        if (!input_test_bit(dev->absbit, abs)) return -EINVAL;
        return copy_to_user(arg, &dev->absinfo[abs], sizeof(struct input_absinfo));
    }

    return -ENOTTY;
}

static const struct inode_ops evdev_ops = {
    .open_file    = evdev_open_file,
    .release_file = evdev_release_file,
    .ioctl_file   = evdev_ioctl,
    .poll_file    = evdev_poll,
    .stream       = true,
    .stream_read  = evdev_read,
    .stream_write = evdev_write,
};

/* ---- the entries of /sys and the udev database ---- */

static void make_dir(const char *path) {
    vfs_mkdir(path, 0755); /* it may be there */
}

/* mkdir -p */
static void make_dirs(const char *path) {
    char buf[256];
    size_t n = strlen(path);

    if (n >= sizeof(buf)) return;

    memcpy(buf, path, n + 1);

    for (size_t i = 1; i < n; i++) {
        if (buf[i] != '/') continue;

        buf[i] = 0;
        make_dir(buf);
        buf[i] = '/';
    }

    make_dir(buf);
}

static void write_file(const char *path, const char *text) {
    struct file *f;

    if (vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644, &f) != 0) return;

    uint64_t pos = 0;

    kernel_write(f, text, strlen(text), &pos);
    vfs_close(f);
}

static void make_link(const char *target, const char *path) {
    vfs_symlink(target, path); /* it may be there */
}

/* "%s" with no formatting library: text at the end of buf */
static size_t put(char *buf, size_t at, size_t cap, const char *s) {
    while (*s && at + 1 < cap) buf[at++] = *s++;

    buf[at] = 0;
    return at;
}

static size_t put_num(char *buf, size_t at, size_t cap, uint64_t v, int base) {
    char digits[24];
    int n = 0;

    do {
        digits[n++] = "0123456789abcdef"[v % base];
        v /= base;
    } while (v);

    while (n && at + 1 < cap) buf[at++] = digits[--n];

    buf[at] = 0;
    return at;
}

/* the kernel's format for a bitmap in sysfs: words of 64 bits from the highest that is not zero, in hex, with spaces */
static size_t put_bitmap(char *buf, size_t at, size_t cap, const uint8_t *bits, size_t nbytes) {
    size_t words = (nbytes + 7) / 8;
    bool started = false;

    for (size_t w = words; w-- > 0;) {
        uint64_t v = 0;

        for (size_t b = 0; b < 8 && w * 8 + b < nbytes; b++) v |= (uint64_t)bits[w * 8 + b] << (8 * b);

        if (!started && !v && w) continue;

        if (started) at = put(buf, at, cap, " ");

        at = put_num(buf, at, cap, v, 16);
        started = true;
    }

    if (!started) at = put(buf, at, cap, "0");
    return at;
}

static bool any_bit(const uint8_t *bits, size_t nbytes) {
    for (size_t i = 0; i < nbytes; i++)
        if (bits[i]) return true;

    return false;
}

#define KEY_ESC   1
#define BTN_MOUSE 0x110
#define BTN_TOUCH 0x14a
#define REL_X 0
#define REL_Y 1
#define ABS_X 0
#define ABS_Y 1

/* what udev's input_id would say of the device: the properties libinput looks for */
static void udev_properties(struct input_dev *dev, char *out, size_t cap) {
    size_t at = 0;
    bool keys = false, keyboard = true, mouse = false, touch = false;

    for (unsigned k = 0; k < BTN_MOUSE; k++)
        if (input_test_bit(dev->keybit, k)) keys = true;

    for (unsigned k = KEY_ESC; k < 32; k++)
        if (!input_test_bit(dev->keybit, k)) keyboard = false; /* all of the first ones, as udev says */

    if (input_test_bit(dev->relbit, REL_X) && input_test_bit(dev->relbit, REL_Y) && input_test_bit(dev->keybit, BTN_MOUSE)) mouse = true;
    /* absolute axes: with a mouse button it is a pointer (a tablet of qemu has the touch button too), else a touch screen */
    if (input_test_bit(dev->absbit, ABS_X) && input_test_bit(dev->absbit, ABS_Y)) {
        if (input_test_bit(dev->keybit, BTN_MOUSE)) mouse = true;
        else if (input_test_bit(dev->keybit, BTN_TOUCH)) touch = true;
    }

    at = put(out, at, cap, "I:1\nE:ID_INPUT=1\n");
    if (keys)     at = put(out, at, cap, "E:ID_INPUT_KEY=1\n");
    if (keyboard) at = put(out, at, cap, "E:ID_INPUT_KEYBOARD=1\n");
    if (mouse)    at = put(out, at, cap, "E:ID_INPUT_MOUSE=1\n");
    if (touch)    at = put(out, at, cap, "E:ID_INPUT_TOUCHSCREEN=1\n");
    put(out, at, cap, "G:seat\nQ:seat\n");
}

static void sysfs_register(struct input_dev *dev) {
    char path[256], target[256], text[512];
    char dir[200], up[64];
    size_t at, n = 0;

    /* ".." for each component of the parent path, and input<n> and event<n> below it */
    at = put(dir, 0, sizeof(dir), "/sys/");
    at = put(dir, at, sizeof(dir), dev->parent_path);

    for (const char *p = dev->parent_path; *p; p++)
        if (*p == '/') n++;

    if (*dev->parent_path) n++;

    up[0] = 0;

    size_t ups = 0;

    for (size_t i = 0; i < n + 2 && ups + 4 < sizeof(up); i++) ups = put(up, ups, sizeof(up), "../");

    /* /sys/<parent>/input/input<i>: the device */
    char idx[8];
    put_num(idx, 0, sizeof(idx), dev->index, 10);

    char input_dir[256];
    at = put(input_dir, 0, sizeof(input_dir), dir);
    at = put(input_dir, at, sizeof(input_dir), "/input/input");
    at = put(input_dir, at, sizeof(input_dir), idx);

    char event_dir[256];
    at = put(event_dir, 0, sizeof(event_dir), input_dir);
    at = put(event_dir, at, sizeof(event_dir), "/event");
    at = put(event_dir, at, sizeof(event_dir), idx);

    make_dirs(event_dir);

    /* attributes */
    #define FILE_IN(dirp, name) (put(path, put(path, 0, sizeof(path), dirp), sizeof(path), "/" name))

    FILE_IN(input_dir, "name");
    at = put(text, 0, sizeof(text), dev->name);
    put(text, at, sizeof(text), "\n");
    write_file(path, text);

    FILE_IN(input_dir, "phys");
    at = put(text, 0, sizeof(text), dev->phys);
    put(text, at, sizeof(text), "\n");
    write_file(path, text);

    FILE_IN(input_dir, "uniq");
    write_file(path, "\n");

    FILE_IN(input_dir, "properties");
    at = put_bitmap(text, 0, sizeof(text), dev->propbit, sizeof(dev->propbit));
    put(text, at, sizeof(text), "\n");
    write_file(path, text);

    FILE_IN(input_dir, "id");
    make_dir(path);

    struct { const char *name; uint16_t value; } ids[] = {
        { "bustype", dev->bustype }, { "vendor", dev->vendor }, { "product", dev->product }, { "version", dev->version },
    };

    for (int i = 0; i < 4; i++) {
        at = put(path, put(path, 0, sizeof(path), input_dir), sizeof(path), "/id/");
        put(path, at, sizeof(path), ids[i].name);

        char hex[8];
        put_num(hex, 0, sizeof(hex), ids[i].value, 16);

        char four[8] = "0000";
        size_t len = strlen(hex);

        memcpy(four + 4 - len, hex, len + 1);
        four[4] = 0;
        put(text, put(text, 0, sizeof(text), four), sizeof(text), "\n");
        write_file(path, text);
    }

    FILE_IN(input_dir, "capabilities");
    make_dir(path);

    struct { const char *name; const uint8_t *bits; size_t size; } caps[] = {
        { "ev", dev->evbit, sizeof(dev->evbit) }, { "key", dev->keybit, sizeof(dev->keybit) },
        { "rel", dev->relbit, sizeof(dev->relbit) }, { "abs", dev->absbit, sizeof(dev->absbit) },
        { "msc", dev->mscbit, sizeof(dev->mscbit) }, { "led", dev->ledbit, sizeof(dev->ledbit) },
        { "sw", dev->swbit, sizeof(dev->swbit) },
    };

    for (int i = 0; i < 7; i++) {
        at = put(path, put(path, 0, sizeof(path), input_dir), sizeof(path), "/capabilities/");
        put(path, at, sizeof(path), caps[i].name);

        at = put_bitmap(text, 0, sizeof(text), caps[i].bits, caps[i].size);
        put(text, at, sizeof(text), "\n");
        write_file(path, text);
    }

    /* uevent: the lines a program reads to know what the device is */
    FILE_IN(input_dir, "uevent");
    at = put(text, 0, sizeof(text), "PRODUCT=");
    at = put_num(text, at, sizeof(text), dev->bustype, 16);
    at = put(text, at, sizeof(text), "/");
    at = put_num(text, at, sizeof(text), dev->vendor, 16);
    at = put(text, at, sizeof(text), "/");
    at = put_num(text, at, sizeof(text), dev->product, 16);
    at = put(text, at, sizeof(text), "/");
    at = put_num(text, at, sizeof(text), dev->version, 16);
    at = put(text, at, sizeof(text), "\nNAME=\"");
    at = put(text, at, sizeof(text), dev->name);
    at = put(text, at, sizeof(text), "\"\nPHYS=\"");
    at = put(text, at, sizeof(text), dev->phys);
    at = put(text, at, sizeof(text), "\"\nPROP=");
    at = put_bitmap(text, at, sizeof(text), dev->propbit, sizeof(dev->propbit));
    at = put(text, at, sizeof(text), "\nEV=");
    at = put_bitmap(text, at, sizeof(text), dev->evbit, sizeof(dev->evbit));

    if (any_bit(dev->keybit, sizeof(dev->keybit))) {
        at = put(text, at, sizeof(text), "\nKEY=");
        at = put_bitmap(text, at, sizeof(text), dev->keybit, sizeof(dev->keybit));
    }

    if (any_bit(dev->relbit, sizeof(dev->relbit))) {
        at = put(text, at, sizeof(text), "\nREL=");
        at = put_bitmap(text, at, sizeof(text), dev->relbit, sizeof(dev->relbit));
    }

    if (any_bit(dev->absbit, sizeof(dev->absbit))) {
        at = put(text, at, sizeof(text), "\nABS=");
        at = put_bitmap(text, at, sizeof(text), dev->absbit, sizeof(dev->absbit));
    }

    put(text, at, sizeof(text), "\n");
    write_file(path, text);

    /* subsystem: input */
    FILE_IN(input_dir, "subsystem");
    put(target, put(target, 0, sizeof(target), up), sizeof(target), "class/input");
    make_link(target, path);

    /* the event node's directory */
    FILE_IN(event_dir, "dev");
    at = put_num(text, 0, sizeof(text), EVDEV_MAJOR, 10);
    at = put(text, at, sizeof(text), ":");
    at = put_num(text, at, sizeof(text), EVDEV_MINOR_BASE + dev->index, 10);
    put(text, at, sizeof(text), "\n");
    write_file(path, text);

    FILE_IN(event_dir, "uevent");
    at = put(text, 0, sizeof(text), "MAJOR=13\nMINOR=");
    at = put_num(text, at, sizeof(text), EVDEV_MINOR_BASE + dev->index, 10);
    at = put(text, at, sizeof(text), "\nDEVNAME=input/event");
    at = put(text, at, sizeof(text), idx);
    put(text, at, sizeof(text), "\n");
    write_file(path, text);

    FILE_IN(event_dir, "subsystem");
    put(target, put(target, 0, sizeof(target), up), sizeof(target), "../class/input"); /* one deeper than the device */
    make_link(target, path);

    FILE_IN(event_dir, "device");
    make_link("..", path);

    /* /sys/class/input, /sys/dev/char */
    make_dirs("/sys/class/input");
    make_dirs("/sys/dev/char");

    char rel[256];

    at = put(path, 0, sizeof(path), "/sys/class/input/input");
    put(path, at, sizeof(path), idx);
    at = put(rel, 0, sizeof(rel), "../../");
    at = put(rel, at, sizeof(rel), dev->parent_path);
    at = put(rel, at, sizeof(rel), "/input/input");
    put(rel, at, sizeof(rel), idx);
    make_link(rel, path);

    at = put(path, 0, sizeof(path), "/sys/class/input/event");
    put(path, at, sizeof(path), idx);
    at = put(rel, strlen(rel), sizeof(rel), "/event");
    put(rel, at, sizeof(rel), idx);
    make_link(rel, path);

    at = put(path, 0, sizeof(path), "/sys/dev/char/13:");
    put_num(path, at, sizeof(path), EVDEV_MINOR_BASE + dev->index, 10);
    at = put(target, 0, sizeof(target), "../../");
    at = put(target, at, sizeof(target), dev->parent_path);
    at = put(target, at, sizeof(target), "/input/input");
    at = put(target, at, sizeof(target), idx);
    at = put(target, at, sizeof(target), "/event");
    put(target, at, sizeof(target), idx);
    make_link(target, path);

    /* the udev database: what a udev rule would have made of it. the input device has one too, by its name ("+input:
     * input0"), and libudev asks for it again and again if there is none */
    make_dirs("/run/udev/data");

    at = put(path, 0, sizeof(path), "/run/udev/data/c13:");
    put_num(path, at, sizeof(path), EVDEV_MINOR_BASE + dev->index, 10);
    udev_properties(dev, text, sizeof(text));
    write_file(path, text);

    at = put(path, 0, sizeof(path), "/run/udev/data/+input:input");
    put(path, at, sizeof(path), idx);
    write_file(path, text);

    /* and the devices above it, which have a uevent file in linux: libudev looks for it on each going up */
    char up_dir[200];
    size_t prefix = strlen("/sys/");

    for (size_t i = prefix; dir[i]; i++) {
        if (dir[i] != '/') continue;

        memcpy(up_dir, dir, i);
        up_dir[i] = 0;

        if (i > strlen("/sys/devices")) {
            at = put(path, 0, sizeof(path), up_dir);
            put(path, at, sizeof(path), "/uevent");
            write_file(path, "\n");
        }
    }

    at = put(path, 0, sizeof(path), dir);
    put(path, at, sizeof(path), "/uevent");
    write_file(path, "\n");
}

int input_register(struct input_dev *dev) {
    dev->index = next_index++;

    spin_lock_init(&dev->lock);
    list_init(&dev->clients);

    input_set_bit(dev->evbit, EV_SYN);

    make_dir("/dev/input");

    struct inode *inode = inode_alloc(&evdev_ops, S_IFCHR | 0660);
    if (!inode) return -ENOMEM;

    inode->priv = dev;
    inode->rdev = EVDEV_MAJOR << 8 | (EVDEV_MINOR_BASE + dev->index);

    char path[32];
    size_t at = put(path, 0, sizeof(path), "/dev/input/event");
    put_num(path, at, sizeof(path), dev->index, 10);

    int err = vfs_link_inode(path, inode);

    iput(inode);

    if (err) return err;

    sysfs_register(dev);

    printk("input: %s, event%d\n", dev->name, dev->index);
    return 0;
}
