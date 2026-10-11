#include <serika/printk.h>
#include <serika/stat.h>
#include <serika/vfs.h>

static long console_read(struct inode *inode, void *buf, size_t n, uint64_t pos) {
    (void)inode; (void)buf; (void)n; (void)pos;

    return 0;
}

/* under the inode lock*/
static long console_write(struct inode *inode, const void *buf, size_t n, uint64_t pos) {
    (void)inode; (void)pos;

    printk("%.*s", (int)n, (const char *)buf);
    return n;
}

static const struct inode_ops console_ops = {
    .read  = console_read,
    .write = console_write,
};

struct inode *console_create(void) {
    return inode_alloc(&console_ops, S_IFCHR | 0620);
}
