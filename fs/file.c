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

#define TCGETS     0x5401
#define TCGETS2    0x802c542a
#define TIOCGWINSZ 0x5413

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

    kfree(files);
}

/* the lowest free fd takes our reference */
static int fd_install(struct files *files, struct file *f) {
    int fd = -EMFILE;

    uint64_t flags = spin_lock_irqsave(&files->lock);

    for (int i = 0; i < FILES_MAX; i++) {
        if (!files->fd[i]) {
            files->fd[i] = f;
            fd = i;
            break;
        }
    }

    spin_unlock_irqrestore(&files->lock, flags);
    return fd;
}

/* a reference of our own: another thread may close the fd meanwhile */
static struct file *fd_get(long fd) {
    struct files *files = current_task()->files;
    struct file *f = NULL;

    if (fd < 0 || fd >= FILES_MAX) return NULL;

    uint64_t flags = spin_lock_irqsave(&files->lock);

    f = files->fd[fd];
    if (f) fget(f);

    spin_unlock_irqrestore(&files->lock, flags);
    return f;
}

/* cwd is "/" until there is one: a relative path is from the root */
static int path_from_user(long dirfd, uintptr_t upath, char **out) {
    char *buf = kmalloc(PATH_MAX);
    if (!buf) return -ENOMEM;

    long len = strncpy_from_user(buf + 1, upath, PATH_MAX - 1);

    if (len == 0) len = -ENOENT;

    if (len >= 0 && buf[1] != '/' && dirfd != AT_FDCWD) {
        printk("syscall: a path from dirfd %ld not supported -- task %lu\n", dirfd, current_task()->id);
        len = -ENOSYS;
    }

    if (len < 0) {
        kfree(buf);
        return len;
    }

    if (buf[1] == '/') memmove(buf, buf + 1, len + 1);
    else               buf[0] = '/';

    *out = buf;
    return 0;
}

long sys_openat(long dirfd, long upath, long flags, long mode) {
    char *path;
    struct file *f;

    int err = path_from_user(dirfd, upath, &path);
    if (err) return err;

    err = vfs_open(path, flags, mode, &f);
    kfree(path);
    if (err) return err;

    int fd = fd_install(current_task()->files, f);
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

/* data goes through a bounce page: no inode lock is held while user memory is touched */
long sys_read(long fd, long buf, long count) {
    if (count < 0) return -EINVAL;

    struct file *f = fd_get(fd);
    if (!f) return -EBADF;

    uintptr_t bounce = (uintptr_t)alloc_pages(0);
    if (!bounce) {
        vfs_close(f);
        return -ENOMEM;
    }

    long done = 0;

    mutex_lock(&f->pos_lock);

    while (done < count) {
        size_t n = count - done;
        if (n > PAGE_SIZE) n = PAGE_SIZE;

        long r = kernel_read(f, (void *)P2V(bounce), n, &f->pos);
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
        if ((size_t)r < n) break; /* the end, or a console with nothing more */
    }

    mutex_unlock(&f->pos_lock);

    free_pages((void *)bounce, 0);
    vfs_close(f);
    return done;
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

    mutex_lock(&f->pos_lock);

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

    mutex_unlock(&f->pos_lock);

    free_pages((void *)bounce, 0);
    vfs_close(f);
    return done;
}

long sys_lseek(long fd, long off, long whence) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;

    struct inode *inode = f->inode;
    long ret;

    if (S_ISCHR(inode->mode)) {
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
    st->st_rdev    = S_ISCHR(inode->mode) ? 0x501 : 0; /* the console is 5:1 */
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

    err = vfs_open(path, O_RDONLY, 0, &f);
    kfree(path);
    if (err) return err;

    struct stat st;
    fill_stat(f->inode, &st);
    vfs_close(f);

    return copy_to_user(ubuf, &st, sizeof(st));
}

/* the console answers as a tty, so glibc buffers stdout by line */
long sys_ioctl(long fd, long cmd, long arg) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;

    bool tty = S_ISCHR(f->inode->mode);
    vfs_close(f);

    if (!tty) return -ENOTTY;

    if (cmd == TCGETS || cmd == TCGETS2) {
        /* struct termios2, the kernel's: cooked, echo, 38400 8n1. termios is its first 36 bytes */
        struct { uint32_t iflag, oflag, cflag, lflag; uint8_t line, cc[19]; uint32_t ispeed, ospeed; } t = {
            .iflag  = 0x0100, /* ICRNL */
            .oflag  = 0x0005, /* OPOST | ONLCR */
            .cflag  = 0x00bf, /* B38400 | CS8 | CREAD */
            .lflag  = 0x8a3b, /* ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE | IEXTEN */
            .ispeed = 38400,
            .ospeed = 38400,
        };

        return copy_to_user(arg, &t, cmd == TCGETS ? 36 : sizeof(t));
    }

    if (cmd == TIOCGWINSZ) {
        struct { uint16_t row, col, xpixel, ypixel; } ws = { 24, 80, 0, 0 };

        return copy_to_user(arg, &ws, sizeof(ws));
    }

    return -ENOTTY;
}
