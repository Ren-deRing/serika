#pragma once

#include <serika/mutex.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NAME_MAX 255
#define PATH_MAX 4096

struct inode;
struct file;
struct poll_table;

/* called with that inode's lock held, and no other inode's. kernel buffers only */
struct inode_ops {
    int  (*lookup)(struct inode *dir, const char *name, size_t len, struct inode **out); /* referenced */
    int  (*create)(struct inode *dir, const char *name, size_t len, uint32_t mode, struct inode **out);
    int  (*symlink)(struct inode *dir, const char *name, size_t len, const char *target, struct inode **out);
    long (*readlink)(struct inode *inode, char *buf, size_t n); /* the target, up to n, no nul. its length */
    int  (*link)(struct inode *dir, const char *name, size_t len, struct inode *inode); /* a name for an inode that exists */
    int  (*unlink)(struct inode *dir, const char *name, size_t len, struct inode *inode); /* the name, if it still is inode's. its reference is the caller's to drop */
    uintptr_t (*getpage)(struct inode *inode, uint64_t index); /* a mapped file's page, a frame with a reference taken. 0: past the end */
    bool map_wc; /* its pages are mapped write-combining: a framebuffer */
    int  (*dir_retire)(struct inode *dir); /* empty: nothing can be put in it again. -ENOTEMPTY */
    long (*read)(struct inode *inode, void *buf, size_t n, uint64_t pos);
    long (*write)(struct inode *inode, const void *buf, size_t n, uint64_t pos);
    long (*ioctl)(struct inode *inode, unsigned long cmd, unsigned long arg); /* no lock held: arg is user memory */
    int  (*truncate)(struct inode *inode, uint64_t size);
    int  (*punch_hole)(struct inode *inode, uint64_t off, uint64_t len); /* what is read there is zeros, the size stays. inside the size. lock held */
    long (*getdents)(struct inode *dir, void *buf, size_t n, uint64_t *pos); /* linux_dirent64s, pos is the entry */
    void (*evict)(struct inode *inode); /* last reference gone, no lock held */
    void (*mark_dirty)(struct inode *inode); /* nlink, the mode, a time or the size changed: to be written. lock held, cheap */

    uint32_t (*poll)(struct inode *inode, uint32_t fflags, struct poll_table *pt); /* POLL* ready now. no lock held. none: always ready */

    /* a stream (a pipe, a tty) sleeps in its reads and writes, so those run without the inode lock and take the
     * file, which says whether to wait (O_NONBLOCK). its read and write ops are not used */
    bool stream;
    long (*stream_read)(struct file *f, void *buf, size_t n);
    long (*stream_write)(struct file *f, const void *buf, size_t n);

    int  (*open)(struct inode *inode, uint32_t flags);    /* a file is being opened: may sleep, no lock held. not for O_PATH */
    void (*release)(struct inode *inode, uint32_t flags); /* a file on it is closed, flags are its. no lock held */

    /* for a device that keeps something for each open file (f->priv): these take the file, and the ones above do not.
     * open_file runs once the file exists, an error closes it; release_file runs with the last reference, before release */
    /* a symlink that is not a path but a way to the file itself (/proc/self/fd/N): the inode it opens, referenced. an
     * open of it that follows links gives that */
    int      (*follow_link)(struct inode *link, struct inode **target);

    int      (*open_file)(struct file *f);
    void     (*release_file)(struct file *f);
    long     (*ioctl_file)(struct file *f, unsigned long cmd, unsigned long arg); /* instead of ioctl */
    uint32_t (*poll_file)(struct file *f, struct poll_table *pt);                 /* instead of poll */
};

struct inode {
    uint64_t     ino;
    uint32_t     dev;   /* which file system, for st_dev: 0 the ramfs. with the ino it is the file */
    uint32_t     mode;  /* S_IF* | permissions */
    uint32_t     nlink;
    uint32_t     rdev;  /* a device: major << 8 | minor */
    uint64_t     atime; /* ns since 1970. lock. reads do not update it */
    uint64_t     mtime;
    uint64_t     ctime;
    uint64_t     size;  /* lock */
    uint32_t     refs;  /* atomic */

    struct mutex lock;  /* size, data, directory entries */

    const struct inode_ops *ops;
    void        *priv;  /* fs data */

    /* mounts. a dir with a file system on it has its root in mounted: a lookup of the dir gives that instead, and
     * ".." of the root goes to the dir its mount point is in */
    struct inode *mounted;
    struct inode *mnt_parent;
};

struct file {
    struct inode *inode;    /* our reference */
    uint32_t      flags;    /* O_* */
    uint32_t      refs;     /* atomic */
    uint64_t      pos;      /* pos_lock */
    struct mutex  pos_lock; /* across a whole read or write, user copies included */
    void         *priv;     /* what open_file() of the inode's ops made */
    char         *path;     /* as it was opened: what a path relative to a directory as a dirfd starts with, and /proc/self/fd/N reads */
};

struct cwd;

/* a task's fds. threads share it */
#define FILES_MAX 256

struct files {
    spinlock_t    lock;  /* fd. nothing is taken under it */
    uint32_t      users; /* atomic */
    struct file  *fd[FILES_MAX];
    uint64_t      cloexec[FILES_MAX / 64]; /* closed by execve */

    /* the fs state rides along: threads share it like Linux's CLONE_FS, fork copies it */
    struct cwd   *cwd;   /* NULL: "/" */
    uint32_t      umask;
};

/* inode->dev of the file systems that are not the ramfs (0) or a disk (1, 2, ...) */
#define VFS_DEV_PROC 100

extern struct inode *vfs_root;
extern struct inode *console_inode; /* fd 0, 1 and 2. the only tty */

void vfs_init(void);
void ext4_init(void); /* after vfs_init() */

struct inode *inode_alloc(const struct inode_ops *ops, uint32_t mode); /* one reference */
void iget(struct inode *inode);
void iput(struct inode *inode);

/* absolute paths only: path_from_user() puts the cwd in front */
int  vfs_open(const char *path, uint32_t flags, uint32_t mode, struct file **out);
void vfs_close(struct file *f); /* drops a reference */
void fget(struct file *f);
uint32_t vfs_poll(struct file *f, struct poll_table *pt); /* POLL* ready now; a file with no poll op is always ready */
bool vfs_pollable(struct file *f);                       /* has a poll op */
void flock_release(struct file *f); /* fs/flock.c. the file's last reference is going: its flock is let go */
struct file *file_alloc(struct inode *inode, uint32_t flags); /* takes the inode's reference */
int  vfs_mkdir(const char *path, uint32_t mode);
int  vfs_symlink(const char *target, const char *path);
int  vfs_mkfifo(const char *path, uint32_t mode);
int  vfs_unlink(const char *path);
int  vfs_rmdir(const char *path);
int  vfs_rename(const char *oldpath, const char *newpath);
int  vfs_link(const char *oldpath, const char *newpath); /* a hard link */
int  vfs_chmod(struct inode *inode, uint32_t mode);
int  vfs_truncate(struct inode *inode, uint64_t size);
int  vfs_punch_hole(struct inode *inode, uint64_t off, uint64_t len); /* fallocate(PUNCH_HOLE) */
long vfs_readlink(struct inode *inode, char *buf, size_t n);
void inode_touch(struct inode *inode); /* its data or entries changed: mtime and ctime, lock held */
void inode_dirty(struct inode *inode); /* the fs is told that its inode changed, lock held */

#define UTIME_NOW  ((1L << 30) - 1)
#define UTIME_OMIT ((1L << 30) - 2)

/* utimensat: ns since 1970, or UTIME_NOW or UTIME_OMIT for either */
int  vfs_utimes(struct inode *inode, int64_t atime_ns, int64_t mtime_ns);
int  vfs_link_inode(const char *path, struct inode *inode); /* /dev nodes: a name in a dir */

/* one linux_dirent64 into buf. its size, 0 if n is too small */
size_t vfs_dirent_put(void *buf, size_t n, uint64_t ino, uint64_t off, uint32_t mode, const char *name, size_t len);
long kernel_getdents(struct file *f, void *buf, size_t n, uint64_t *pos);

/* kernel buffers. the caller keeps the position */
long kernel_read(struct file *f, void *buf, size_t n, uint64_t *pos);
long kernel_write(struct file *f, const void *buf, size_t n, uint64_t *pos);

/* source: a block device by name ("sda", or "/dev/sda"). only "ext4"; its changes are still refused */
int vfs_mount(const char *source, const char *target, const char *type);
int vfs_umount(const char *target); /* an ext4 that nothing has open. 0, -EINVAL (not the root of a mount), -EBUSY */
size_t vfs_mounts_text(char *buf, size_t cap, bool info); /* /proc/mounts, or mountinfo */
struct inode *proc_root(void); /* the root of a procfs, referenced */

struct inode *ramfs_root(void);
struct inode *ramfs_anon_file(void); /* a file with no name: memfd, shared anonymous memory */
uintptr_t vfs_getpage(struct inode *inode, uint64_t index);
struct inode *console_create(void);
void mem_devices_create(void);
void tty_dev_create(void);     /* /dev/tty: the caller's controlling terminal, the console if there is none */
void pty_devices_create(void); /* /dev/ptmx and /dev/pts */
void fb_devices_create(void); /* /dev/fb0, if the boot gave a framebuffer */
void drm_devices_create(void); /* /dev/dri/card0, on the same framebuffer */
int pipe_create(struct file *out[2], uint32_t flags); /* read end, write end. flags: O_NONBLOCK */
struct inode *fifo_create(uint32_t mode);              /* a named pipe: link it somewhere */
void initramfs_unpack(void);

struct files *files_create(void); /* 0, 1 and 2 on the console */
void files_get(struct files *files);
size_t files_cwd(struct files *files, char *out, size_t max);
void files_put(struct files *files);
struct files *files_copy(struct files *old);
int fd_install(struct files *files, struct file *f, bool cloexec, int min); /* the lowest free fd from min takes our reference */
struct file *fd_get(long fd); /* the current task's, referenced. vfs_close() when done */
int  anon_fd(struct inode *inode, uint32_t flags, bool cloexec); /* a nameless file on inode (eventfd, epoll) in the current task. takes the reference, which is dropped on failure */
void files_close_on_exec(struct files *files);

int path_from_kernel(const char *s, char **out); /* the same, for a string in the kernel */
int path_from_user(long dirfd, uintptr_t upath, char **out);
