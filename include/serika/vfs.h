#pragma once

#include <serika/mutex.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NAME_MAX 255
#define PATH_MAX 4096

struct inode;
struct poll_table;

/* called with that inode's lock held, and no other inode's. kernel buffers only */
struct inode_ops {
    int  (*lookup)(struct inode *dir, const char *name, size_t len, struct inode **out); /* referenced */
    int  (*create)(struct inode *dir, const char *name, size_t len, uint32_t mode, struct inode **out);
    int  (*symlink)(struct inode *dir, const char *name, size_t len, const char *target, struct inode **out);
    long (*readlink)(struct inode *inode, char *buf, size_t n); /* the target, up to n, no nul. its length */
    int  (*link)(struct inode *dir, const char *name, size_t len, struct inode *inode); /* a name for an inode that exists */
    int  (*unlink)(struct inode *dir, const char *name, size_t len, struct inode *inode); /* the name, if it still is inode's. its reference is the caller's to drop */
    int  (*dir_retire)(struct inode *dir); /* empty: nothing can be put in it again. -ENOTEMPTY */
    long (*read)(struct inode *inode, void *buf, size_t n, uint64_t pos);
    long (*write)(struct inode *inode, const void *buf, size_t n, uint64_t pos);
    long (*ioctl)(struct inode *inode, unsigned long cmd, unsigned long arg); /* no lock held: arg is user memory */
    int  (*truncate)(struct inode *inode, uint64_t size);
    long (*getdents)(struct inode *dir, void *buf, size_t n, uint64_t *pos); /* linux_dirent64s, pos is the entry */
    void (*evict)(struct inode *inode); /* last reference gone, no lock held */

    /* a stream sleeps in read and write, so they run without the inode lock and take care of their own */
    uint32_t (*poll)(struct inode *inode, uint32_t fflags, struct poll_table *pt); /* POLL* ready now. no lock held. none: always ready */
    bool stream;
    void (*release)(struct inode *inode, uint32_t flags); /* a file on it is closed, flags are its. no lock held */
};

struct inode {
    uint64_t     ino;
    uint32_t     mode;  /* S_IF* | permissions */
    uint32_t     nlink;
    uint32_t     rdev;  /* a device: major << 8 | minor */
    uint64_t     size;  /* lock */
    uint32_t     refs;  /* atomic */

    struct mutex lock;  /* size, data, directory entries */

    const struct inode_ops *ops;
    void        *priv;  /* fs data */
};

struct file {
    struct inode *inode;    /* our reference */
    uint32_t      flags;    /* O_* */
    uint32_t      refs;     /* atomic */
    uint64_t      pos;      /* pos_lock */
    struct mutex  pos_lock; /* across a whole read or write, user copies included */
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

extern struct inode *vfs_root;
extern struct inode *console_inode; /* fd 0, 1 and 2. the only tty */

void vfs_init(void);

struct inode *inode_alloc(const struct inode_ops *ops, uint32_t mode); /* one reference */
void iget(struct inode *inode);
void iput(struct inode *inode);

/* absolute paths only: path_from_user() puts the cwd in front */
int  vfs_open(const char *path, uint32_t flags, uint32_t mode, struct file **out);
void vfs_close(struct file *f); /* drops a reference */
void fget(struct file *f);
struct file *file_alloc(struct inode *inode, uint32_t flags); /* takes the inode's reference */
int  vfs_mkdir(const char *path, uint32_t mode);
int  vfs_symlink(const char *target, const char *path);
int  vfs_unlink(const char *path);
int  vfs_rmdir(const char *path);
int  vfs_rename(const char *oldpath, const char *newpath);
int  vfs_link(const char *oldpath, const char *newpath); /* a hard link */
int  vfs_chmod(struct inode *inode, uint32_t mode);
int  vfs_truncate(struct inode *inode, uint64_t size);
long vfs_readlink(struct inode *inode, char *buf, size_t n);
int  vfs_link_inode(const char *path, struct inode *inode); /* /dev nodes: a name in a dir */

/* one linux_dirent64 into buf. its size, 0 if n is too small */
size_t vfs_dirent_put(void *buf, size_t n, uint64_t ino, uint64_t off, uint32_t mode, const char *name, size_t len);
long kernel_getdents(struct file *f, void *buf, size_t n, uint64_t *pos);

/* kernel buffers. the caller keeps the position */
long kernel_read(struct file *f, void *buf, size_t n, uint64_t *pos);
long kernel_write(struct file *f, const void *buf, size_t n, uint64_t *pos);

struct inode *ramfs_root(void);
struct inode *console_create(void);
void mem_devices_create(void);
int pipe_create(struct file *out[2]); /* read end, write end */
void initramfs_unpack(void);

struct files *files_create(void); /* 0, 1 and 2 on the console */
void files_get(struct files *files);
void files_put(struct files *files);
struct files *files_copy(struct files *old);
struct file *fd_get(long fd); /* the current task's, referenced. vfs_close() when done */
void files_close_on_exec(struct files *files);

int path_from_user(long dirfd, uintptr_t upath, char **out);
