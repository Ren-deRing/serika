#pragma once

#include <serika/mutex.h>

#include <stddef.h>
#include <stdint.h>

#define NAME_MAX 255
#define PATH_MAX 4096

struct inode;

/* called with that inode's lock held, and no other inode's. kernel buffers only */
struct inode_ops {
    int  (*lookup)(struct inode *dir, const char *name, size_t len, struct inode **out); /* referenced */
    int  (*create)(struct inode *dir, const char *name, size_t len, uint32_t mode, struct inode **out);
    long (*read)(struct inode *inode, void *buf, size_t n, uint64_t pos);
    long (*write)(struct inode *inode, const void *buf, size_t n, uint64_t pos);
    int  (*truncate)(struct inode *inode, uint64_t size);
    void (*evict)(struct inode *inode); /* last reference gone, no lock held */
};

struct inode {
    uint64_t     ino;
    uint32_t     mode;  /* S_IF* | permissions */
    uint32_t     nlink;
    uint64_t     size;  /* lock */
    uint32_t     refs;  /* atomic */

    struct mutex lock;  /* size, data, directory entries */

    const struct inode_ops *ops;
    void        *priv;  /* fs data */
};

struct file {
    struct inode *inode; /* our reference */
    uint32_t      flags; /* O_* */
    uint32_t      refs;  /* atomic */
};

extern struct inode *vfs_root;

void vfs_init(void);

struct inode *inode_alloc(const struct inode_ops *ops, uint32_t mode); /* one reference */
void iget(struct inode *inode);
void iput(struct inode *inode);

/* absolute paths only, no cwd yet */
int  vfs_open(const char *path, uint32_t flags, uint32_t mode, struct file **out);
void vfs_close(struct file *f);
int  vfs_mkdir(const char *path, uint32_t mode);

/* kernel buffers. the caller keeps the position */
long kernel_read(struct file *f, void *buf, size_t n, uint64_t *pos);
long kernel_write(struct file *f, const void *buf, size_t n, uint64_t *pos);

struct inode *ramfs_root(void);
void initramfs_unpack(void);
