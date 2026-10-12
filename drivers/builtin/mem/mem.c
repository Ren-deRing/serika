#include <serika/errno.h>
#include <serika/panic.h>
#include <serika/random.h>
#include <serika/stat.h>
#include <serika/string.h>
#include <serika/vfs.h>

/* /dev/null, zero, full, random, urandom. no state: the inode lock is held for nothing */

static long null_read(struct inode *inode, void *buf, size_t n, uint64_t pos) {
    (void)inode; (void)buf; (void)n; (void)pos;

    return 0;
}

static long null_write(struct inode *inode, const void *buf, size_t n, uint64_t pos) {
    (void)inode; (void)buf; (void)pos;

    return n;
}

static long zero_read(struct inode *inode, void *buf, size_t n, uint64_t pos) {
    (void)inode; (void)pos;

    memset(buf, 0, n);
    return n;
}

static long full_write(struct inode *inode, const void *buf, size_t n, uint64_t pos) {
    (void)inode; (void)buf; (void)n; (void)pos;

    return -ENOSPC;
}

/* the generator never runs dry, so random and urandom are the same */
static long random_read(struct inode *inode, void *buf, size_t n, uint64_t pos) {
    (void)inode; (void)pos;

    get_random_bytes(buf, n);
    return n;
}

static const struct inode_ops null_ops   = { .read = null_read,   .write = null_write };
static const struct inode_ops zero_ops   = { .read = zero_read,   .write = null_write };
static const struct inode_ops full_ops   = { .read = zero_read,   .write = full_write };
static const struct inode_ops random_ops = { .read = random_read, .write = null_write };

/* the device numbers are linux's, which is what stat shows */
static const struct {
    const char             *path;
    const struct inode_ops *ops;
    uint32_t                rdev;
} devices[] = {
    { "/dev/null",    &null_ops,   0x103 },
    { "/dev/zero",    &zero_ops,   0x105 },
    { "/dev/full",    &full_ops,   0x107 },
    { "/dev/random",  &random_ops, 0x108 },
    { "/dev/urandom", &random_ops, 0x109 },
};

/* after /dev is there */
void mem_devices_create(void) {
    for (size_t i = 0; i < sizeof(devices) / sizeof(devices[0]); i++) {
        struct inode *inode = inode_alloc(devices[i].ops, S_IFCHR | 0666);
        if (!inode) panic("mem: unable to create %s -- out of memory?", devices[i].path);

        inode->rdev = devices[i].rdev;

        int err = vfs_link_inode(devices[i].path, inode);
        if (err) panic("mem: unable to create %s -- error %d", devices[i].path, err);

        iput(inode); /* the name has its own */
    }
}
