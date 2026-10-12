#include <serika/errno.h>
#include <serika/fcntl.h>
#include <serika/page.h>
#include <serika/panic.h>
#include <serika/slab.h>
#include <serika/stat.h>
#include <serika/string.h>
#include <serika/timer.h>
#include <serika/vfs.h>

struct inode *vfs_root;
struct inode *console_inode;

static volatile uint64_t next_ino = 1;

/* rename and rmdir move or remove directories: one at a time, so a tree never loses its root or loops */
static struct mutex rename_mutex;

void vfs_init(void) {
    mutex_init(&rename_mutex);

    vfs_root = ramfs_root();
    if (!vfs_root) panic("vfs: unable to create the root -- out of memory?");

    console_inode = console_create();
    if (!console_inode) panic("vfs: no console tty -- is the serial driver up?");

    /* one tty: what the console, /dev/tty and the uart are */
    static const char *const names[] = { "/dev/console", "/dev/tty", "/dev/ttyS0" };

    if (vfs_mkdir("/dev", 0755) != 0) panic("vfs: unable to create /dev -- out of memory?");

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (vfs_link_inode(names[i], console_inode) != 0) panic("vfs: unable to create %s -- out of memory?", names[i]);

    mem_devices_create();
    fb_devices_create();
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

    inode->atime = inode->mtime = inode->ctime = realtime_ns();

    return inode;
}

void inode_touch(struct inode *inode) {
    inode->mtime = inode->ctime = realtime_ns();
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

#define SYMLINK_MAX 40

long vfs_readlink(struct inode *inode, char *buf, size_t n) {
    if (!S_ISLNK(inode->mode) || !inode->ops->readlink) return -EINVAL;

    mutex_lock(&inode->lock);
    long r = inode->ops->readlink(inode, buf, n);
    mutex_unlock(&inode->lock);

    return r;
}

/* work holds a path whose component before rest is the link. it becomes the link's target followed by rest.
 * 1 if the target starts at the root, 0 if at the dir the link is in */
static int splice_link(char *work, const char *rest, struct inode *link) {
    char *tmp = kmalloc(PATH_MAX);
    if (!tmp) return -ENOMEM;

    long len = vfs_readlink(link, tmp, PATH_MAX - 1);
    size_t rest_len = strlen(rest);

    if (len <= 0 || len + rest_len >= PATH_MAX) {
        kfree(tmp);
        return len <= 0 ? -ENOENT : -ENAMETOOLONG;
    }

    memcpy(tmp + len, rest, rest_len + 1); /* rest is in work: before it is written over */
    memcpy(work, tmp, len + rest_len + 1);

    int abs = tmp[0] == '/';

    kfree(tmp);
    return abs;
}

/* a reference on the dir holding the last name of work, which is a PATH_MAX buffer and is rewritten where a link
 * is followed: *last points into it. "/" has none: len 0. cur, a reference, is where it starts and is consumed.
 * the last name itself is not followed */
static int walk_parent(struct inode *cur, char *work, const char **last, size_t *last_len, struct inode **dir,
                       int *links) {
    const char *name;
    size_t len;
    const char *rest = next_name(work, &name, &len);

    for (;;) {
        const char *next_n;
        size_t next_len;
        const char *next_rest = next_name(rest, &next_n, &next_len);

        if (next_len == 0) break; /* name is the last */

        struct inode *next;
        int err = step(cur, name, len, &next);

        if (err) {
            iput(cur);
            return err;
        }

        if (S_ISLNK(next->mode)) {
            if (++*links > SYMLINK_MAX) err = -ELOOP;
            else err = splice_link(work, rest, next);

            iput(next);

            if (err < 0) {
                iput(cur);
                return err;
            }

            if (err == 1) {
                iput(cur);
                cur = vfs_root;
                iget(cur);
            }

            rest = next_name(work, &name, &len);
            continue;
        }

        iput(cur);

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

/* walk_parent() from the root, on a copy of path: *work is the caller's to free */
static int walk_path(const char *path, char **work, struct inode **dir, const char **last, size_t *len,
                     int *links) {
    if (path[0] != '/') return -ENOENT;
    if (strnlen(path, PATH_MAX) == PATH_MAX) return -ENAMETOOLONG;

    *work = kmalloc(PATH_MAX);
    if (!*work) return -ENOMEM;

    strcpy(*work, path);

    iget(vfs_root);

    int err = walk_parent(vfs_root, *work, last, len, dir, links);
    if (err) kfree(*work);

    return err;
}

int vfs_open(const char *path, uint32_t flags, uint32_t mode, struct file **out) {
    struct inode *dir, *inode = NULL;
    const char *name;
    size_t len;
    char *work;
    int links = 0;

    int err = walk_path(path, &work, &dir, &name, &len, &links);
    if (err) return err;

    for (;;) {
        if (len == 0) {
            inode = dir; /* "/" */
            dir = NULL;
            break;
        }

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

        if (err || !S_ISLNK(inode->mode)) break;

        /* a link is followed from the dir it is in, unless asked to stop at it */
        if (flags & O_NOFOLLOW) {
            if (!(flags & O_PATH)) err = -ELOOP;
            break;
        }

        err = ++links > SYMLINK_MAX ? -ELOOP : splice_link(work, "", inode);

        iput(inode);
        inode = NULL;

        if (err < 0) break;

        struct inode *start = dir;
        dir = NULL;

        if (err == 1) {
            iput(start);
            start = vfs_root;
            iget(start);
        }

        err = walk_parent(start, work, &name, &len, &dir, &links);
        if (err) break;
    }

    if (dir) iput(dir);
    kfree(work);

    if (!err && (flags & O_DIRECTORY) && !S_ISDIR(inode->mode)) err = -ENOTDIR;
    if (!err && S_ISDIR(inode->mode) && (flags & O_ACCMODE) != O_RDONLY) err = -EISDIR;

    if (!err && (flags & O_TRUNC) && S_ISREG(inode->mode) && (flags & O_ACCMODE) != O_RDONLY) {
        mutex_lock(&inode->lock);
        err = inode->ops->truncate ? inode->ops->truncate(inode, 0) : -EINVAL;
        if (!err) inode_touch(inode);
        mutex_unlock(&inode->lock);
    }

    /* a fifo waits here for the other end. an O_PATH file is not an open one */
    bool opened = false;

    if (!err && !(flags & O_PATH) && inode->ops->open) {
        err = inode->ops->open(inode, flags);
        opened = !err;
    }

    struct file *f = NULL;
    if (!err && !(f = file_alloc(inode, flags))) err = -ENOMEM;

    if (err) {
        if (opened && inode->ops->release) inode->ops->release(inode, flags);
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

    if (f->inode->ops->release && !(f->flags & O_PATH)) f->inode->ops->release(f->inode, f->flags);

    iput(f->inode);
    kfree(f);
}

/* name in path is made in its dir by make(). an existing one is EEXIST */
static int vfs_make(const char *path, int (*make)(struct inode *dir, const char *name, size_t len, void *arg),
                    void *arg) {
    struct inode *dir, *found = NULL;
    const char *name;
    size_t len;
    char *work;
    int links = 0;

    int err = walk_path(path, &work, &dir, &name, &len, &links);
    if (err) return err;

    if (len == 0) err = -EEXIST; /* "/" */
    else if (!S_ISDIR(dir->mode)) err = -ENOTDIR;
    else if (len > NAME_MAX) err = -ENAMETOOLONG;

    if (!err) {
        mutex_lock(&dir->lock);

        err = lookup_locked(dir, name, len, &found);

        if (err == -ENOENT) err = make(dir, name, len, arg);
        else if (!err)      err = -EEXIST;

        mutex_unlock(&dir->lock);
    }

    iput(dir);
    if (found) iput(found);

    kfree(work);
    return err;
}

static int make_dir(struct inode *dir, const char *name, size_t len, void *arg) {
    struct inode *inode;

    int err = dir->ops->create(dir, name, len, S_IFDIR | (*(uint32_t *)arg & 07777), &inode);
    if (!err) iput(inode);

    return err;
}

int vfs_mkdir(const char *path, uint32_t mode) {
    return vfs_make(path, make_dir, &mode);
}

static int make_symlink(struct inode *dir, const char *name, size_t len, void *arg) {
    struct inode *inode;

    if (!dir->ops->symlink) return -EPERM;

    int err = dir->ops->symlink(dir, name, len, arg, &inode);
    if (!err) iput(inode);

    return err;
}

int vfs_symlink(const char *target, const char *path) {
    return vfs_make(path, make_symlink, (void *)target);
}

static int make_link(struct inode *dir, const char *name, size_t len, void *arg) {
    if (!dir->ops->link) return -EPERM;

    return dir->ops->link(dir, name, len, arg);
}

int vfs_link_inode(const char *path, struct inode *inode) {
    return vfs_make(path, make_link, inode);
}

/* a path's dir and name, and what the name is if anything: all referenced, nothing locked */
struct entry {
    char         *work;
    struct inode *dir;
    struct inode *inode; /* NULL: the name is free */
    const char   *name;
    size_t        len;
};

static void entry_put(struct entry *e) {
    iput(e->dir);
    if (e->inode) iput(e->inode);
    kfree(e->work);
}

/* "." and ".." are not names to remove or move: dots_err */
static int entry_get(const char *path, struct entry *e, int dots_err) {
    int links = 0;

    int err = walk_path(path, &e->work, &e->dir, &e->name, &e->len, &links);
    if (err) return err;

    e->inode = NULL;

    if (e->len == 0)                                   err = -EBUSY; /* "/" */
    else if (!S_ISDIR(e->dir->mode))                   err = -ENOTDIR;
    else if (e->len > NAME_MAX)                        err = -ENAMETOOLONG;
    else if (e->name[0] == '.' && (e->len == 1 || (e->len == 2 && e->name[1] == '.'))) err = dots_err;

    if (!err) {
        mutex_lock(&e->dir->lock);
        err = lookup_locked(e->dir, e->name, e->len, &e->inode);
        mutex_unlock(&e->dir->lock);

        if (err == -ENOENT) err = 0;
    }

    if (err) {
        iput(e->dir);
        kfree(e->work);
    }

    return err;
}

/* the name has left its dir: its reference goes, and one link of the inode */
static void drop_link(struct inode *inode) {
    mutex_lock(&inode->lock);
    if (inode->nlink) inode->nlink--;
    mutex_unlock(&inode->lock);

    iput(inode);
}

/* e's name out of its dir, if it is still e's inode */
static int entry_unlink(struct entry *e) {
    if (!e->dir->ops->unlink) return -EPERM;

    mutex_lock(&e->dir->lock);
    int err = e->dir->ops->unlink(e->dir, e->name, e->len, e->inode);
    mutex_unlock(&e->dir->lock);

    return err;
}

int vfs_unlink(const char *path) {
    struct entry e;

    int err = entry_get(path, &e, -EISDIR);
    if (err) return err;

    if (!e.inode)                    err = -ENOENT;
    else if (S_ISDIR(e.inode->mode)) err = -EISDIR;
    else if (!(err = entry_unlink(&e))) drop_link(e.inode);

    entry_put(&e);
    return err;
}

/* the dir is empty and stays so: the lock of the dir itself is all it takes, and rename_mutex keeps it where it is */
static int retire(struct inode *dir) {
    if (!dir->ops->dir_retire) return -EPERM;

    mutex_lock(&dir->lock);
    int err = dir->ops->dir_retire(dir);
    mutex_unlock(&dir->lock);

    return err;
}

int vfs_rmdir(const char *path) {
    struct entry e;

    mutex_lock(&rename_mutex);

    int err = entry_get(path, &e, -EINVAL);
    if (err) goto out;

    if (!e.inode)                     err = -ENOENT;
    else if (!S_ISDIR(e.inode->mode)) err = -ENOTDIR;
    else if (!(err = retire(e.inode)) && !(err = entry_unlink(&e))) iput(e.inode); /* the name's. nlink is 0 already */

    entry_put(&e);
out:
    mutex_unlock(&rename_mutex);
    return err;
}

/* is dir ancestor, or below it. one dir locked at a time on the way up */
static bool is_inside(struct inode *ancestor, struct inode *dir) {
    struct inode *cur = dir;

    iget(cur);

    for (;;) {
        if (cur == ancestor) {
            iput(cur);
            return true;
        }

        struct inode *up;

        mutex_lock(&cur->lock);
        int err = lookup_locked(cur, "..", 2, &up);
        mutex_unlock(&cur->lock);

        if (err || up == cur) { /* the root is its own */
            if (!err) iput(up);
            iput(cur);
            return false;
        }

        iput(cur);
        cur = up;
    }
}

static int do_rename(struct entry *o, struct entry *n) {
    struct inode *src = o->inode, *dst = n->inode;
    int err;

    if (!src) return -ENOENT;
    if (src == dst) return 0;

    bool dir = S_ISDIR(src->mode);

    if (dst && S_ISDIR(dst->mode) != dir) return dir ? -ENOTDIR : -EISDIR;
    if (dir && is_inside(src, n->dir)) return -EINVAL;
    if (!n->dir->ops->link) return -EPERM;

    /* what the new name was is gone first. a dir has to be empty */
    if (dst) {
        if (dir && (err = retire(dst))) return err;
        if ((err = entry_unlink(n))) return err;

        /* the name's reference. the one lookup gave is entry_put()'s */
        if (dir) iput(dst);
        else     drop_link(dst);
    }

    mutex_lock(&n->dir->lock);
    err = n->dir->ops->link(n->dir, n->name, n->len, src);
    mutex_unlock(&n->dir->lock);

    if (err) return err;

    err = entry_unlink(o);
    if (!err) drop_link(src);

    return err;
}

int vfs_rename(const char *oldpath, const char *newpath) {
    struct entry o, n;

    mutex_lock(&rename_mutex);

    int err = entry_get(oldpath, &o, -EBUSY);
    if (err) goto out;

    if ((err = entry_get(newpath, &n, -EBUSY))) {
        entry_put(&o);
        goto out;
    }

    err = do_rename(&o, &n);

    entry_put(&n);
    entry_put(&o);
out:
    mutex_unlock(&rename_mutex);
    return err;
}

int vfs_mkfifo(const char *path, uint32_t mode) {
    struct inode *inode = fifo_create(mode);
    if (!inode) return -ENOMEM;

    int err = vfs_link_inode(path, inode);

    iput(inode); /* the name has its own */
    return err;
}

/* newpath becomes another name for what oldpath is. not for a dir */
int vfs_link(const char *oldpath, const char *newpath) {
    struct file *f;

    int err = vfs_open(oldpath, O_PATH | O_NOFOLLOW, 0, &f);
    if (err) return err;

    if (S_ISDIR(f->inode->mode)) err = -EPERM;
    else                         err = vfs_link_inode(newpath, f->inode);

    vfs_close(f);
    return err;
}

int vfs_chmod(struct inode *inode, uint32_t mode) {
    mutex_lock(&inode->lock);
    inode->mode = (inode->mode & S_IFMT) | (mode & 07777);
    inode->ctime = realtime_ns();
    mutex_unlock(&inode->lock);

    return 0;
}

int vfs_truncate(struct inode *inode, uint64_t size) {
    if (!S_ISREG(inode->mode)) return S_ISDIR(inode->mode) ? -EISDIR : -EINVAL;
    if (!inode->ops->truncate) return -EINVAL;

    mutex_lock(&inode->lock);
    int err = inode->ops->truncate(inode, size);
    if (!err) inode_touch(inode);
    mutex_unlock(&inode->lock);

    return err;
}

int vfs_utimes(struct inode *inode, int64_t atime_ns, int64_t mtime_ns) {
    mutex_lock(&inode->lock);

    uint64_t now = realtime_ns();

    if (atime_ns != UTIME_OMIT) inode->atime = atime_ns == UTIME_NOW ? now : (uint64_t)atime_ns;
    if (mtime_ns != UTIME_OMIT) inode->mtime = mtime_ns == UTIME_NOW ? now : (uint64_t)mtime_ns;

    inode->ctime = now;

    mutex_unlock(&inode->lock);
    return 0;
}

/* mm->lock is held and the inode lock is taken: the order a file fault has always had */
uintptr_t vfs_getpage(struct inode *inode, uint64_t index) {
    if (!inode->ops->getpage) return 0;

    mutex_lock(&inode->lock);
    uintptr_t phys = inode->ops->getpage(inode, index);
    mutex_unlock(&inode->lock);

    return phys;
}

long kernel_read(struct file *f, void *buf, size_t n, uint64_t *pos) {
    struct inode *inode = f->inode;

    if ((f->flags & O_ACCMODE) == O_WRONLY || (f->flags & O_PATH)) return -EBADF;
    if (S_ISDIR(inode->mode)) return -EISDIR;
    if (inode->ops->stream) return inode->ops->stream_read(f, buf, n);

    if (!inode->ops->read) return -EINVAL;

    mutex_lock(&inode->lock);
    long r = inode->ops->read(inode, buf, n, *pos);
    mutex_unlock(&inode->lock);

    if (r > 0) *pos += r;
    return r;
}

long kernel_write(struct file *f, const void *buf, size_t n, uint64_t *pos) {
    struct inode *inode = f->inode;

    if ((f->flags & O_ACCMODE) == O_RDONLY || (f->flags & O_PATH)) return -EBADF;
    if (inode->ops->stream) return inode->ops->stream_write(f, buf, n);

    if (!inode->ops->write) return -EINVAL;

    mutex_lock(&inode->lock);

    uint64_t at = (f->flags & O_APPEND) ? inode->size : *pos;
    long r = (at + n < at) ? -EFBIG : inode->ops->write(inode, buf, n, at);

    if (r > 0) inode_touch(inode);

    mutex_unlock(&inode->lock);

    if (r > 0) *pos = at + r;
    return r;
}

/* the dirent type is the mode's S_IFMT >> 12, which is what DT_* are */
struct dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];
};

size_t vfs_dirent_put(void *buf, size_t n, uint64_t ino, uint64_t off, uint32_t mode, const char *name, size_t len) {
    size_t reclen = ALIGN_UP(sizeof(struct dirent64) + len + 1, 8);
    if (reclen > n) return 0;

    struct dirent64 *d = buf;

    d->d_ino    = ino;
    d->d_off    = off;
    d->d_reclen = reclen;
    d->d_type   = (mode & S_IFMT) >> 12;

    memcpy(d->d_name, name, len);
    memset(d->d_name + len, 0, reclen - sizeof(*d) - len);

    return reclen;
}

long kernel_getdents(struct file *f, void *buf, size_t n, uint64_t *pos) {
    struct inode *inode = f->inode;

    if (!S_ISDIR(inode->mode) || !inode->ops->getdents) return -ENOTDIR;

    mutex_lock(&inode->lock);
    long r = inode->ops->getdents(inode, buf, n, pos);
    mutex_unlock(&inode->lock);

    return r;
}
