#include <serika/errno.h>
#include <serika/fcntl.h>
#include <serika/panic.h>
#include <serika/slab.h>
#include <serika/stat.h>
#include <serika/string.h>
#include <serika/vfs.h>

struct inode *vfs_root;
struct inode *console_inode;

static volatile uint64_t next_ino = 1;

void vfs_init(void) {
    vfs_root = ramfs_root();
    if (!vfs_root) panic("vfs: unable to create the root -- out of memory?");

    console_inode = console_create();
    if (!console_inode) panic("vfs: unable to create the console -- out of memory?");
}

struct inode *inode_alloc(const struct inode_ops *ops, uint32_t mode) {
    struct inode *inode = kmalloc(sizeof(struct inode));
    if (!inode) return NULL;

    memset(inode, 0, sizeof(*inode));

    inode->ino  = __atomic_fetch_add(&next_ino, 1, 0); /* __ATOMIC_RELAXED */
    inode->mode = mode;
    inode->refs = 1;
    inode->ops  = ops;
    mutex_init(&inode->lock);

    return inode;
}

void iget(struct inode *inode) {
    __atomic_add_fetch(&inode->refs, 1, 0); /* __ATOMIC_RELAXED */
}

/* never with an inode lock held: the last one evicts */
void iput(struct inode *inode) {
    if (__atomic_sub_fetch(&inode->refs, 1, 4) != 0) return; /* __ATOMIC_ACQ_REL */

    if (inode->ops->evict) inode->ops->evict(inode);
    kfree(inode);
}

/* the next component. returns what is after it */
static const char *next_name(const char *path, const char **name, size_t *len) {
    while (*path == '/') path++;

    *name = path;
    while (*path && *path != '/') path++;
    *len = path - *name;

    return path;
}

/* dir locked */
static int lookup_locked(struct inode *dir, const char *name, size_t len, struct inode **out) {
    if (len == 1 && name[0] == '.') {
        iget(dir);
        *out = dir;
        return 0;
    }

    return dir->ops->lookup(dir, name, len, out); /* ".." is the fs's */
}

/* one lock at a time: dir is unlocked before we step into what we found */
static int step(struct inode *dir, const char *name, size_t len, struct inode **out) {
    if (!S_ISDIR(dir->mode)) return -ENOTDIR;
    if (len > NAME_MAX) return -ENAMETOOLONG;

    mutex_lock(&dir->lock);
    int err = lookup_locked(dir, name, len, out);
    mutex_unlock(&dir->lock);

    return err;
}

/* a reference on the dir holding the last name. "/" has none: len 0 */
static int walk_parent(const char *path, struct inode **dir, const char **last, size_t *last_len) {
    if (path[0] != '/') return -ENOENT;
    if (strnlen(path, PATH_MAX) == PATH_MAX) return -ENAMETOOLONG;

    struct inode *cur = vfs_root;
    iget(cur);

    const char *name;
    size_t len;
    const char *rest = next_name(path, &name, &len);

    for (;;) {
        const char *next_n;
        size_t next_len;
        const char *next_rest = next_name(rest, &next_n, &next_len);

        if (next_len == 0) break; /* name is the last */

        struct inode *next;
        int err = step(cur, name, len, &next);

        iput(cur);
        if (err) return err;

        cur  = next;
        name = next_n;
        len  = next_len;
        rest = next_rest;
    }

    *dir = cur;
    *last = name;
    *last_len = len;
    return 0;
}

int vfs_open(const char *path, uint32_t flags, uint32_t mode, struct file **out) {
    struct inode *dir, *inode = NULL;
    const char *name;
    size_t len;

    int err = walk_parent(path, &dir, &name, &len);
    if (err) return err;

    if (len == 0) {
        inode = dir; /* "/" */
    } else {
        if (!S_ISDIR(dir->mode)) err = -ENOTDIR;
        else if (len > NAME_MAX) err = -ENAMETOOLONG;

        /* lookup and create under one lock: O_EXCL holds */
        if (!err) {
            mutex_lock(&dir->lock);

            err = lookup_locked(dir, name, len, &inode);

            if (err == -ENOENT && (flags & O_CREAT))
                err = dir->ops->create(dir, name, len, S_IFREG | (mode & 07777), &inode);
            else if (!err && (flags & O_CREAT) && (flags & O_EXCL))
                err = -EEXIST;

            mutex_unlock(&dir->lock);
        }

        iput(dir);
    }

    if (!err && (flags & O_DIRECTORY) && !S_ISDIR(inode->mode)) err = -ENOTDIR;
    if (!err && S_ISDIR(inode->mode) && (flags & O_ACCMODE) != O_RDONLY) err = -EISDIR;

    if (!err && (flags & O_TRUNC) && S_ISREG(inode->mode) && (flags & O_ACCMODE) != O_RDONLY) {
        mutex_lock(&inode->lock);
        err = inode->ops->truncate ? inode->ops->truncate(inode, 0) : -EINVAL;
        mutex_unlock(&inode->lock);
    }

    struct file *f = NULL;
    if (!err && !(f = file_alloc(inode, flags))) err = -ENOMEM;

    if (err) {
        if (inode) iput(inode);
        return err;
    }

    *out = f;
    return 0;
}

/* NULL on failure, and the reference is still the caller's */
struct file *file_alloc(struct inode *inode, uint32_t flags) {
    struct file *f = kmalloc(sizeof(struct file));
    if (!f) return NULL;

    f->inode = inode;
    f->flags = flags;
    f->refs  = 1;
    f->pos   = 0;
    mutex_init(&f->pos_lock);

    return f;
}

void fget(struct file *f) {
    __atomic_add_fetch(&f->refs, 1, 0); /* __ATOMIC_RELAXED */
}

void vfs_close(struct file *f) {
    if (__atomic_sub_fetch(&f->refs, 1, 4) != 0) return; /* __ATOMIC_ACQ_REL */

    iput(f->inode);
    kfree(f);
}

int vfs_mkdir(const char *path, uint32_t mode) {
    struct inode *dir, *inode = NULL;
    const char *name;
    size_t len;

    int err = walk_parent(path, &dir, &name, &len);
    if (err) return err;

    if (len == 0) err = -EEXIST; /* "/" */
    else if (!S_ISDIR(dir->mode)) err = -ENOTDIR;
    else if (len > NAME_MAX) err = -ENAMETOOLONG;

    if (!err) {
        mutex_lock(&dir->lock);

        err = lookup_locked(dir, name, len, &inode);

        if (err == -ENOENT) err = dir->ops->create(dir, name, len, S_IFDIR | (mode & 07777), &inode);
        else if (!err)      err = -EEXIST;

        mutex_unlock(&dir->lock);
    }

    iput(dir);
    if (inode) iput(inode);

    return err;
}

long kernel_read(struct file *f, void *buf, size_t n, uint64_t *pos) {
    struct inode *inode = f->inode;

    if ((f->flags & O_ACCMODE) == O_WRONLY) return -EBADF;
    if (S_ISDIR(inode->mode)) return -EISDIR;
    if (!inode->ops->read) return -EINVAL;

    mutex_lock(&inode->lock);
    long r = inode->ops->read(inode, buf, n, *pos);
    mutex_unlock(&inode->lock);

    if (r > 0) *pos += r;
    return r;
}

long kernel_write(struct file *f, const void *buf, size_t n, uint64_t *pos) {
    struct inode *inode = f->inode;

    if ((f->flags & O_ACCMODE) == O_RDONLY) return -EBADF;
    if (!inode->ops->write) return -EINVAL;

    mutex_lock(&inode->lock);

    uint64_t at = (f->flags & O_APPEND) ? inode->size : *pos;
    long r = (at + n < at) ? -EFBIG : inode->ops->write(inode, buf, n, at);

    mutex_unlock(&inode->lock);

    if (r > 0) *pos = at + r;
    return r;
}
