#include <serika/buddy.h>
#include <serika/cpu.h>
#include <serika/errno.h>
#include <serika/fcntl.h>
#include <serika/mm.h>
#include <serika/page.h>
#include <serika/printk.h>
#include <serika/slab.h>
#include <serika/stat.h>
#include <serika/string.h>
#include <serika/syscall.h>
#include <serika/task.h>
#include <serika/vfs.h>

/* never changed once made: chdir swaps the pointer */
struct cwd {
    uint32_t refs; /* atomic */
    char     path[];
};

static void cwd_put(struct cwd *cwd) {
    if (cwd && __atomic_sub_fetch(&cwd->refs, 1, 4) == 0) kfree(cwd); /* __ATOMIC_ACQ_REL */
}

struct files *files_create(void) {
    struct files *files = kmalloc(sizeof(struct files));
    if (!files) return NULL;

    iget(console_inode); /* the file's */

    struct file *console = file_alloc(console_inode, O_RDWR);
    if (!console) {
        iput(console_inode);
        kfree(files);
        return NULL;
    }

    memset(files, 0, sizeof(*files));
    spin_lock_init(&files->lock);
    files->users = 1;
    files->umask = 022;

    fget(console);
    fget(console);

    files->fd[0] = console;
    files->fd[1] = console;
    files->fd[2] = console;

    return files;
}

void files_get(struct files *files) {
    __atomic_add_fetch(&files->users, 1, 0); /* __ATOMIC_RELAXED */
}

/* task context: the last one closes everything */
void files_put(struct files *files) {
    if (__atomic_sub_fetch(&files->users, 1, 4) != 0) return; /* __ATOMIC_ACQ_REL */

    for (int i = 0; i < FILES_MAX; i++)
        if (files->fd[i]) vfs_close(files->fd[i]);

    cwd_put(files->cwd);
    kfree(files);
}

/* files->lock held */
static void cloexec_set(struct files *files, int fd, bool on) {
    if (on) files->cloexec[fd / 64] |= 1ULL << (fd % 64);
    else    files->cloexec[fd / 64] &= ~(1ULL << (fd % 64));
}

/* the lowest free fd from min takes our reference */
static int fd_install(struct files *files, struct file *f, bool cloexec, int min) {
    int fd = -EMFILE;

    uint64_t flags = spin_lock_irqsave(&files->lock);

    for (int i = min; i < FILES_MAX; i++) {
        if (!files->fd[i]) {
            files->fd[i] = f;
            fd = i;

            cloexec_set(files, i, cloexec);
            break;
        }
    }

    spin_unlock_irqrestore(&files->lock, flags);
    return fd;
}

/* a reference of our own: another thread may close the fd meanwhile */
struct file *fd_get(long fd) {
    struct files *files = current_task()->files;
    struct file *f = NULL;

    if (fd < 0 || fd >= FILES_MAX) return NULL;

    uint64_t flags = spin_lock_irqsave(&files->lock);

    f = files->fd[fd];
    if (f) fget(f);

    spin_unlock_irqrestore(&files->lock, flags);
    return f;
}

/* "name" becomes "/cwd/name" */
static long cwd_prepend(char *buf, long len) {
    struct files *files = current_task()->files;
    long ret;

    uint64_t flags = spin_lock_irqsave(&files->lock);

    const char *cwd = files->cwd ? files->cwd->path : "/";
    size_t cwd_len = strlen(cwd);

    if (cwd_len + 1 + len >= PATH_MAX) {
        ret = -ENAMETOOLONG;
    } else {
        memmove(buf + cwd_len + 1, buf, len + 1);
        memcpy(buf, cwd, cwd_len);
        buf[cwd_len] = '/';
        ret = cwd_len + 1 + len;
    }

    spin_unlock_irqrestore(&files->lock, flags);
    return ret;
}

/* a relative path starts at the cwd. no directory fds as a start yet */
int path_from_user(long dirfd, uintptr_t upath, char **out) {
    char *buf = kmalloc(PATH_MAX);
    if (!buf) return -ENOMEM;

    long len = strncpy_from_user(buf, upath, PATH_MAX - 1);

    if (len == 0) len = -ENOENT;

    if (len > 0 && buf[0] != '/') {
        if (dirfd != AT_FDCWD) {
            printk("syscall: a path from dirfd %ld not supported -- task %lu\n", dirfd, current_task()->id);
            len = -ENOSYS;
        } else {
            len = cwd_prepend(buf, len);
        }
    }

    if (len < 0) {
        kfree(buf);
        return len;
    }

    *out = buf;
    return 0;
}

long sys_openat(long dirfd, long upath, long flags, long mode) {
    char *path;
    struct file *f;

    int err = path_from_user(dirfd, upath, &path);
    if (err) return err;

    err = vfs_open(path, flags, mode & ~current_task()->files->umask, &f);
    kfree(path);
    if (err) return err;

    int fd = fd_install(current_task()->files, f, flags & O_CLOEXEC, 0);
    if (fd < 0) vfs_close(f);

    return fd;
}

long sys_close(long fd) {
    struct files *files = current_task()->files;
    struct file *f = NULL;

    if (fd < 0 || fd >= FILES_MAX) return -EBADF;

    uint64_t flags = spin_lock_irqsave(&files->lock);

    f = files->fd[fd];
    files->fd[fd] = NULL;

    spin_unlock_irqrestore(&files->lock, flags);

    if (!f) return -EBADF;

    vfs_close(f); /* after the lock: it may evict */
    return 0;
}

/* no position: a pipe or a tty */
static bool is_stream(struct inode *inode) {
    return S_ISCHR(inode->mode) || S_ISFIFO(inode->mode);
}

/* data goes through a bounce page: no inode lock is held while user memory is touched */
static long read_to_user(struct file *f, long buf, long count, uint64_t *pos) {
    uintptr_t bounce = (uintptr_t)alloc_pages(0);
    if (!bounce) return -ENOMEM;

    long done = 0;

    while (done < count) {
        size_t n = count - done;
        if (n > PAGE_SIZE) n = PAGE_SIZE;

        long r = kernel_read(f, (void *)P2V(bounce), n, pos);
        if (r <= 0) {
            if (!done) done = r;
            break;
        }

        int err = copy_to_user(buf + done, (void *)P2V(bounce), r);
        if (err) {
            if (!done) done = err;
            break;
        }

        done += r;
        if ((size_t)r < n || is_stream(f->inode)) break; /* the end, or what a stream had to give */
    }

    free_pages((void *)bounce, 0);
    return done;
}

long sys_read(long fd, long buf, long count) {
    if (count < 0) return -EINVAL;

    struct file *f = fd_get(fd);
    if (!f) return -EBADF;

    /* a stream has no position, and a reader asleep on it must not hold a writer of the same file up */
    bool lock = !is_stream(f->inode);

    if (lock) mutex_lock(&f->pos_lock);
    long ret = read_to_user(f, buf, count, &f->pos);
    if (lock) mutex_unlock(&f->pos_lock);

    vfs_close(f);
    return ret;
}

/* at off, the file's position untouched: no pos_lock */
long sys_pread64(long fd, long buf, long count, long off) {
    if (count < 0 || off < 0) return -EINVAL;

    struct file *f = fd_get(fd);
    if (!f) return -EBADF;

    uint64_t pos = off;
    long ret = is_stream(f->inode) ? -ESPIPE : read_to_user(f, buf, count, &pos);

    vfs_close(f);
    return ret;
}


long sys_write(long fd, long buf, long count) {
    if (count < 0) return -EINVAL;

    struct file *f = fd_get(fd);
    if (!f) return -EBADF;

    uintptr_t bounce = (uintptr_t)alloc_pages(0);
    if (!bounce) {
        vfs_close(f);
        return -ENOMEM;
    }

    long done = 0;
    bool lock = !is_stream(f->inode);

    if (lock) mutex_lock(&f->pos_lock);

    while (done < count) {
        size_t n = count - done;
        if (n > PAGE_SIZE) n = PAGE_SIZE;

        int err = copy_from_user((void *)P2V(bounce), buf + done, n);
        if (err) {
            if (!done) done = err;
            break;
        }

        long r = kernel_write(f, (void *)P2V(bounce), n, &f->pos);
        if (r <= 0) {
            if (!done) done = r;
            break;
        }

        done += r;
        if ((size_t)r < n) break;
    }

    if (lock) mutex_unlock(&f->pos_lock);

    free_pages((void *)bounce, 0);
    vfs_close(f);
    return done;
}

long sys_lseek(long fd, long off, long whence) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;

    struct inode *inode = f->inode;
    long ret;

    if (is_stream(inode)) {
        vfs_close(f);
        return -ESPIPE;
    }

    mutex_lock(&f->pos_lock);

    int64_t base;

    if (whence == SEEK_SET) {
        base = 0;
    } else if (whence == SEEK_CUR) {
        base = f->pos;
    } else if (whence == SEEK_END) {
        mutex_lock(&inode->lock);
        base = inode->size;
        mutex_unlock(&inode->lock);
    } else {
        base = -1;
    }

    if (base < 0 || base + off < 0) {
        ret = -EINVAL;
    } else {
        f->pos = base + off;
        ret = f->pos;
    }

    mutex_unlock(&f->pos_lock);
    vfs_close(f);
    return ret;
}

static void fill_stat(struct inode *inode, struct stat *st) {
    memset(st, 0, sizeof(*st));

    mutex_lock(&inode->lock);

    st->st_dev     = 1;
    st->st_ino     = inode->ino;
    st->st_nlink   = inode->nlink;
    st->st_mode    = inode->mode;
    st->st_rdev    = inode->rdev;
    st->st_size    = inode->size;
    st->st_blksize = PAGE_SIZE;
    st->st_blocks  = ALIGN_UP(inode->size, 512) / 512;

    mutex_unlock(&inode->lock);
}

long sys_fstat(long fd, long ubuf) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;

    struct stat st;
    fill_stat(f->inode, &st);
    vfs_close(f);

    return copy_to_user(ubuf, &st, sizeof(st));
}

long sys_newfstatat(long dirfd, long upath, long ubuf, long flags) {
    char *path, first;
    struct file *f;

    /* "" with AT_EMPTY_PATH is dirfd itself */
    if ((flags & AT_EMPTY_PATH) && strncpy_from_user(&first, upath, 1) == 0) return sys_fstat(dirfd, ubuf);

    int err = path_from_user(dirfd, upath, &path);
    if (err) return err;

    err = vfs_open(path, O_PATH | ((flags & AT_SYMLINK_NOFOLLOW) ? O_NOFOLLOW : 0), 0, &f);
    kfree(path);
    if (err) return err;

    struct stat st;
    fill_stat(f->inode, &st);
    vfs_close(f);

    return copy_to_user(ubuf, &st, sizeof(st));
}

/* the device's own: a tty answers termios, the window, the foreground */
long sys_ioctl(long fd, long cmd, long arg) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;

    const struct inode_ops *ops = f->inode->ops;
    long ret = ops->ioctl ? ops->ioctl(f->inode, cmd, arg) : -ENOTTY;

    vfs_close(f);
    return ret;
}

/* fork: the same open files, one more reference each */
struct files *files_copy(struct files *old) {
    struct files *files = kmalloc(sizeof(struct files));
    if (!files) return NULL;

    memset(files, 0, sizeof(*files));
    spin_lock_init(&files->lock);
    files->users = 1;

    uint64_t flags = spin_lock_irqsave(&old->lock);

    for (int i = 0; i < FILES_MAX; i++) {
        files->fd[i] = old->fd[i];
        if (files->fd[i]) fget(files->fd[i]);
    }

    memcpy(files->cloexec, old->cloexec, sizeof(files->cloexec));

    files->umask = old->umask;
    files->cwd = old->cwd;
    if (files->cwd) __atomic_add_fetch(&files->cwd->refs, 1, 0); /* __ATOMIC_RELAXED */

    spin_unlock_irqrestore(&old->lock, flags);
    return files;
}

/* execve: the fds marked close-on-exec go. one at a time: the close comes after the lock */
void files_close_on_exec(struct files *files) {
    for (int i = 0; i < FILES_MAX; i++) {
        uint64_t flags = spin_lock_irqsave(&files->lock);

        struct file *f = NULL;

        if (files->cloexec[i / 64] & (1ULL << (i % 64))) {
            f = files->fd[i];
            files->fd[i] = NULL;
            files->cloexec[i / 64] &= ~(1ULL << (i % 64));
        }

        spin_unlock_irqrestore(&files->lock, flags);

        if (f) vfs_close(f);
    }
}

/* no users or permissions yet: whatever is there may be used */
long sys_faccessat(long dirfd, long upath, long mode) {
    (void)mode;

    char *path;
    struct file *f;

    int err = path_from_user(dirfd, upath, &path);
    if (err) return err;

    err = vfs_open(path, O_RDONLY, 0, &f);
    kfree(path);
    if (err) return err;

    vfs_close(f);
    return 0;
}

long sys_access(long upath, long mode) {
    return sys_faccessat(AT_FDCWD, upath, mode);
}

/* newfd takes a reference of oldfd's file, and what it held is closed */
static long dup_to(long oldfd, long newfd, bool cloexec) {
    struct files *files = current_task()->files;

    if (newfd < 0 || newfd >= FILES_MAX) return -EBADF;

    struct file *f = fd_get(oldfd);
    if (!f) return -EBADF;

    uint64_t flags = spin_lock_irqsave(&files->lock);

    struct file *prev = files->fd[newfd];
    files->fd[newfd] = f;
    cloexec_set(files, newfd, cloexec);

    spin_unlock_irqrestore(&files->lock, flags);

    if (prev) vfs_close(prev);
    return newfd;
}

long sys_dup(long fd) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;

    int ret = fd_install(current_task()->files, f, false, 0);
    if (ret < 0) vfs_close(f);

    return ret;
}

long sys_dup2(long oldfd, long newfd) {
    if (oldfd == newfd) {
        struct file *f = fd_get(oldfd);
        if (!f) return -EBADF;

        vfs_close(f);
        return newfd;
    }

    return dup_to(oldfd, newfd, false);
}

long sys_dup3(long oldfd, long newfd, long flags) {
    if (flags & ~O_CLOEXEC) return -EINVAL;
    if (oldfd == newfd) return -EINVAL;

    return dup_to(oldfd, newfd, flags & O_CLOEXEC);
}

long sys_fcntl(long fd, long cmd, long arg) {
    struct files *files = current_task()->files;
    struct file *f;
    long ret;

    switch (cmd) {
        case F_DUPFD:
        case F_DUPFD_CLOEXEC:
            if (arg < 0 || arg >= FILES_MAX) return -EINVAL;
            if (!(f = fd_get(fd))) return -EBADF;

            ret = fd_install(files, f, cmd == F_DUPFD_CLOEXEC, arg);
            if (ret < 0) vfs_close(f);

            return ret;

        case F_GETFD:
        case F_SETFD: {
            if (fd < 0 || fd >= FILES_MAX) return -EBADF;

            uint64_t flags = spin_lock_irqsave(&files->lock);

            if (!files->fd[fd]) {
                ret = -EBADF;
            } else if (cmd == F_GETFD) {
                ret = (files->cloexec[fd / 64] >> (fd % 64)) & 1 ? FD_CLOEXEC : 0;
            } else {
                cloexec_set(files, fd, arg & FD_CLOEXEC);
                ret = 0;
            }

            spin_unlock_irqrestore(&files->lock, flags);
            return ret;
        }

        case F_GETFL:
            if (!(f = fd_get(fd))) return -EBADF;

            ret = f->flags & ~(O_CLOEXEC | O_CREAT | O_EXCL | O_TRUNC);
            vfs_close(f);
            return ret;

        /* only O_APPEND changes. a nonblocking file is not there yet */
        case F_SETFL:
            if (arg & O_NONBLOCK) {
                printk("syscall: O_NONBLOCK not supported -- task %lu\n", current_task()->id);
                return -EINVAL;
            }

            if (!(f = fd_get(fd))) return -EBADF;

            __atomic_store_n(&f->flags, (f->flags & ~O_APPEND) | (arg & O_APPEND), 0); /* __ATOMIC_RELAXED */
            vfs_close(f);
            return 0;
    }

    printk("syscall: fcntl %ld not supported -- task %lu\n", cmd, current_task()->id);
    return -EINVAL;
}

long sys_pipe2(long ufds, long flags) {
    struct files *files = current_task()->files;
    struct file *f[2];
    int fds[2];

    if (flags & ~O_CLOEXEC) {
        if (flags & O_NONBLOCK) printk("syscall: O_NONBLOCK not supported -- task %lu\n", current_task()->id);
        return -EINVAL;
    }

    int err = pipe_create(f);
    if (err) return err;

    fds[0] = fd_install(files, f[0], flags & O_CLOEXEC, 0);
    if (fds[0] < 0) {
        vfs_close(f[0]);
        vfs_close(f[1]);
        return fds[0];
    }

    fds[1] = fd_install(files, f[1], flags & O_CLOEXEC, 0);
    if (fds[1] < 0) {
        sys_close(fds[0]);
        vfs_close(f[1]);
        return fds[1];
    }

    err = copy_to_user(ufds, fds, sizeof(fds));
    if (err) {
        sys_close(fds[0]);
        sys_close(fds[1]);
        return err;
    }

    return 0;
}

long sys_getdents64(long fd, long ubuf, long count) {
    if (count < 0) return -EINVAL;

    struct file *f = fd_get(fd);
    if (!f) return -EBADF;

    uintptr_t bounce = (uintptr_t)alloc_pages(0);
    if (!bounce) {
        vfs_close(f);
        return -ENOMEM;
    }

    size_t n = count > (long)PAGE_SIZE ? PAGE_SIZE : count;

    mutex_lock(&f->pos_lock);
    long ret = kernel_getdents(f, (void *)P2V(bounce), n, &f->pos);
    mutex_unlock(&f->pos_lock);

    if (ret > 0) {
        int err = copy_to_user(ubuf, (void *)P2V(bounce), ret);
        if (err) ret = err;
    }

    free_pages((void *)bounce, 0);
    vfs_close(f);
    return ret;
}

/* "/a/./b//../c" is "/a/c": there are no links, so ".." is just the parent */
static void path_normalize(char *path) {
    char *out = path;
    const char *p = path;

    for (;;) {
        while (*p == '/') p++;

        const char *name = p;
        while (*p && *p != '/') p++;

        size_t len = p - name;
        if (len == 0) break;

        if (len == 1 && name[0] == '.') continue;

        if (len == 2 && name[0] == '.' && name[1] == '.') {
            while (out > path && *--out != '/') {}
            continue;
        }

        *out++ = '/';
        memmove(out, name, len);
        out += len;
    }

    if (out == path) *out++ = '/';
    *out = '\0';
}

long sys_chdir(long upath) {
    struct files *files = current_task()->files;
    char *path;
    struct file *f;

    int err = path_from_user(AT_FDCWD, upath, &path);
    if (err) return err;

    err = vfs_open(path, O_RDONLY | O_DIRECTORY, 0, &f);
    if (err) {
        kfree(path);
        return err;
    }

    vfs_close(f);
    path_normalize(path);

    size_t len = strlen(path);
    struct cwd *cwd = kmalloc(sizeof(struct cwd) + len + 1);

    if (cwd) {
        cwd->refs = 1;
        memcpy(cwd->path, path, len + 1);
    }

    kfree(path);
    if (!cwd) return -ENOMEM;

    uint64_t flags = spin_lock_irqsave(&files->lock);

    struct cwd *old = files->cwd;
    files->cwd = cwd;

    spin_unlock_irqrestore(&files->lock, flags);

    cwd_put(old);
    return 0;
}

/* the length with the nul, as the system call does */
long sys_getcwd(long ubuf, long size) {
    struct files *files = current_task()->files;

    char *tmp = kmalloc(PATH_MAX);
    if (!tmp) return -ENOMEM;

    uint64_t flags = spin_lock_irqsave(&files->lock);
    strcpy(tmp, files->cwd ? files->cwd->path : "/");
    spin_unlock_irqrestore(&files->lock, flags);

    long len = strlen(tmp) + 1;
    long ret = len;

    if (size < len) {
        ret = -ERANGE;
    } else {
        int err = copy_to_user(ubuf, tmp, len);
        if (err) ret = err;
    }

    kfree(tmp);
    return ret;
}

long sys_mkdirat(long dirfd, long upath, long mode) {
    char *path;

    int err = path_from_user(dirfd, upath, &path);
    if (err) return err;

    err = vfs_mkdir(path, mode & ~current_task()->files->umask);
    kfree(path);
    return err;
}

long sys_umask(long mask) {
    struct files *files = current_task()->files;

    uint64_t flags = spin_lock_irqsave(&files->lock);

    long old = files->umask;
    files->umask = mask & 0777;

    spin_unlock_irqrestore(&files->lock, flags);
    return old;
}

long sys_readlinkat(long dirfd, long upath, long ubuf, long size) {
    char *path, *target;
    struct file *f;

    if (size <= 0) return -EINVAL;

    int err = path_from_user(dirfd, upath, &path);
    if (err) return err;

    err = vfs_open(path, O_PATH | O_NOFOLLOW, 0, &f);
    kfree(path);
    if (err) return err;

    if (!(target = kmalloc(PATH_MAX))) {
        vfs_close(f);
        return -ENOMEM;
    }

    long len = vfs_readlink(f->inode, target, size < PATH_MAX ? size : PATH_MAX);
    vfs_close(f);

    if (len > 0 && copy_to_user(ubuf, target, len) != 0) len = -EFAULT;

    kfree(target);
    return len;
}

long sys_symlinkat(long utarget, long dirfd, long ulink) {
    char *target, *link;

    if (!(target = kmalloc(PATH_MAX))) return -ENOMEM;

    long len = strncpy_from_user(target, utarget, PATH_MAX - 1);
    if (len == 0) len = -ENOENT;

    if (len < 0) {
        kfree(target);
        return len;
    }

    int err = path_from_user(dirfd, ulink, &link);
    if (!err) {
        err = vfs_symlink(target, link);
        kfree(link);
    }

    kfree(target);
    return err;
}

struct iovec {
    uint64_t base;
    uint64_t len;
};

#define IOV_MAX 1024

/* one read or write per vector, as long as each one is filled */
static long iov_rw(long fd, long uiov, long count, long (*rw)(long, long, long)) {
    long total = 0;

    if (count < 0 || count > IOV_MAX) return -EINVAL;

    for (long i = 0; i < count; i++) {
        struct iovec iov;

        int err = copy_from_user(&iov, uiov + i * sizeof(iov), sizeof(iov));
        if (err) return total ? total : err;

        if (!iov.len) continue;

        long r = rw(fd, iov.base, iov.len);
        if (r < 0) return total ? total : r;

        total += r;
        if ((uint64_t)r < iov.len) break;
    }

    return total;
}

long sys_readv(long fd, long uiov, long count) {
    return iov_rw(fd, uiov, count, sys_read);
}

long sys_writev(long fd, long uiov, long count) {
    return iov_rw(fd, uiov, count, sys_write);
}

/* in at its offset (or its own position), straight into out. in must be a file with a position */
long sys_sendfile(long out_fd, long in_fd, long uoff, long count) {
    struct file *in, *out;
    uint64_t off = 0;
    long done = 0;

    if (count < 0) return -EINVAL;
    if (out_fd == in_fd) return -EINVAL;

    if (!(in = fd_get(in_fd))) return -EBADF;
    if (!(out = fd_get(out_fd))) {
        vfs_close(in);
        return -EBADF;
    }

    long ret = 0;
    uintptr_t bounce = 0;

    if (is_stream(in->inode) || (in->flags & O_ACCMODE) == O_WRONLY) ret = -EINVAL;
    else if (uoff && copy_from_user(&off, uoff, sizeof(off)) != 0) ret = -EFAULT;
    else if (!(bounce = (uintptr_t)alloc_pages(0))) ret = -ENOMEM;

    if (ret) goto out;

    bool in_lock = !uoff, out_lock = !is_stream(out->inode);

    if (in_lock)  mutex_lock(&in->pos_lock);
    if (out_lock) mutex_lock(&out->pos_lock);

    uint64_t *pos = uoff ? &off : &in->pos;

    while (done < count) {
        size_t n = count - done;
        if (n > PAGE_SIZE) n = PAGE_SIZE;

        long r = kernel_read(in, (void *)P2V(bounce), n, pos);
        if (r <= 0) {
            if (!done) done = r;
            break;
        }

        long w = kernel_write(out, (void *)P2V(bounce), r, &out->pos);
        if (w < r) {
            *pos -= r - (w > 0 ? w : 0); /* what was read and not written is read again */
            if (w > 0) done += w;
            else if (!done) done = w;
            break;
        }

        done += w;
    }

    if (out_lock) mutex_unlock(&out->pos_lock);
    if (in_lock)  mutex_unlock(&in->pos_lock);

    free_pages((void *)bounce, 0);
    ret = done;

    if (uoff && done > 0 && copy_to_user(uoff, &off, sizeof(off)) != 0) ret = -EFAULT;

out:
    vfs_close(in);
    vfs_close(out);
    return ret;
}

long sys_unlinkat(long dirfd, long upath, long flags) {
    char *path;

    if (flags & ~AT_REMOVEDIR) return -EINVAL;

    int err = path_from_user(dirfd, upath, &path);
    if (err) return err;

    err = (flags & AT_REMOVEDIR) ? vfs_rmdir(path) : vfs_unlink(path);

    kfree(path);
    return err;
}

/* both paths are made before either is used */
static long two_paths(long olddirfd, long uold, long newdirfd, long unew, int (*op)(const char *, const char *)) {
    char *oldp, *newp;

    int err = path_from_user(olddirfd, uold, &oldp);
    if (err) return err;

    err = path_from_user(newdirfd, unew, &newp);
    if (!err) {
        err = op(oldp, newp);
        kfree(newp);
    }

    kfree(oldp);
    return err;
}

long sys_renameat2(long olddirfd, long uold, long newdirfd, long unew, long flags) {
    if (flags) return -EINVAL; /* NOREPLACE, EXCHANGE */

    return two_paths(olddirfd, uold, newdirfd, unew, vfs_rename);
}

long sys_linkat(long olddirfd, long uold, long newdirfd, long unew, long flags) {
    if (flags) return -EINVAL; /* the old name is not followed, which is what 0 gives on linux */

    return two_paths(olddirfd, uold, newdirfd, unew, vfs_link);
}

/* the file at path, not followed if it is a link, or the one fd names */
static int file_at(long dirfd, long upath, long flags, struct file **out) {
    char *path;

    if (!upath) {
        *out = fd_get(dirfd);
        return *out ? 0 : -EBADF;
    }

    int err = path_from_user(dirfd, upath, &path);
    if (err) return err;

    err = vfs_open(path, O_PATH | ((flags & AT_SYMLINK_NOFOLLOW) ? O_NOFOLLOW : 0), 0, out);
    kfree(path);
    return err;
}

long sys_fchmodat(long dirfd, long upath, long mode) {
    struct file *f;

    int err = file_at(dirfd, upath, 0, &f);
    if (err) return err;

    err = vfs_chmod(f->inode, mode);
    vfs_close(f);
    return err;
}

long sys_fchmod(long fd, long mode) {
    return sys_fchmodat(fd, 0, mode);
}

/* there is one user. -1 leaves the id as it is */
long sys_fchownat(long dirfd, long upath, long uid, long gid, long flags) {
    struct file *f;

    int err = file_at(dirfd, upath, flags, &f);
    if (err) return err;

    vfs_close(f);
    return ((uint32_t)uid != (uint32_t)-1 && uid != 0) || ((uint32_t)gid != (uint32_t)-1 && gid != 0) ? -EPERM : 0;
}

long sys_ftruncate(long fd, long len) {
    if (len < 0) return -EINVAL;

    struct file *f = fd_get(fd);
    if (!f) return -EBADF;

    long err = (f->flags & O_ACCMODE) == O_RDONLY || (f->flags & O_PATH) ? -EINVAL : vfs_truncate(f->inode, len);

    vfs_close(f);
    return err;
}

long sys_truncate(long upath, long len) {
    char *path;
    struct file *f;

    if (len < 0) return -EINVAL;

    int err = path_from_user(AT_FDCWD, upath, &path);
    if (err) return err;

    err = vfs_open(path, O_PATH, 0, &f);
    kfree(path);
    if (err) return err;

    err = vfs_truncate(f->inode, len);
    vfs_close(f);
    return err;
}

/* memory only: there is nothing to write out */
long sys_fsync(long fd) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;

    vfs_close(f);
    return 0;
}

/* no timestamps are kept, so there is nothing to set: the file has to be there */
long sys_utimensat(long dirfd, long upath, long utimes, long flags) {
    (void)utimes;

    struct file *f;

    int err = file_at(dirfd, upath, flags, &f);
    if (err) return err;

    vfs_close(f);
    return 0;
}
