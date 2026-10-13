#include <serika/block.h>
#include <serika/errno.h>
#include <serika/fcntl.h>
#include <serika/lock.h>
#include <serika/mm.h>
#include <serika/printk.h>
#include <serika/slab.h>
#include <serika/stat.h>
#include <serika/string.h>
#include <serika/syscall.h>
#include <serika/vfs.h>

#include "ext4/ext4.h"

#define MS_RDONLY 1

/* what is mounted, for /proc/mounts */
#define MOUNTS_MAX 8

static struct {
    char     source[32];
    char     target[48];
    char     type[16];
    uint32_t dev;
    struct inode *dir, *up; /* the mount point and where its ".." goes: the references the mount holds */
} mounts[MOUNTS_MAX];

static int nmounts;
static spinlock_t mounts_lock = SPINLOCK_INITIALIZER;

/* "rootfs / rootfs rw 0 0" and a line for each mount; or mountinfo's: id, parent, device, root, mount point, options, type, source */
size_t vfs_mounts_text(char *buf, size_t cap, bool info) {
    size_t at = 0;

    at += info ? ksnprintf(buf + at, cap - at, "1 0 0:0 / / rw - rootfs rootfs rw\n") :
                 ksnprintf(buf + at, cap - at, "rootfs / rootfs rw 0 0\n");

    uint64_t flags = spin_lock_irqsave(&mounts_lock);

    for (int i = 0; i < nmounts && at < cap; i++) {
        at += info ? ksnprintf(buf + at, cap - at, "%d 1 0:%u / %s ro - %s %s ro\n", 2 + i, 1 + mounts[i].dev, mounts[i].target,
                               mounts[i].type, mounts[i].source) :
                     ksnprintf(buf + at, cap - at, "%s %s %s ro 0 0\n", mounts[i].source, mounts[i].target, mounts[i].type);
    }

    spin_unlock_irqrestore(&mounts_lock, flags);
    return at;
}

/* target becomes the root of the file system on source: an ext4 on a disk, or the /proc. it stays mounted until vfs_umount() */
int vfs_mount(const char *source, const char *target, const char *type) {
    bool proc = strcmp(type, "proc") == 0;
    struct blkdev *dev = NULL;

    if (!proc && strcmp(type, "ext4") != 0) return -ENODEV;

    if (!proc) {
        if (strncmp(source, "/dev/", 5) == 0) source += 5;

        dev = blk_find(source);
        if (!dev) return -ENOENT;
    }

    struct file *mp, *up;

    int err = vfs_open(target, O_PATH, 0, &mp);
    if (err) return err;

    struct inode *dir = mp->inode;

    if (!S_ISDIR(dir->mode)) err = -ENOTDIR;
    else if (dir == vfs_root || dir->mounted || dir->mnt_parent) err = -EBUSY;
    else if (nmounts >= MOUNTS_MAX) err = -ENOSPC; /* it could not be listed, and so not taken off */

    char *up_path = err ? NULL : kmalloc(PATH_MAX);

    if (!err && !up_path) err = -ENOMEM;

    /* where its ".." goes once something is mounted on it */
    if (!err) {
        if (strlen(target) + 3 >= PATH_MAX) {
            err = -ENAMETOOLONG;
        } else {
            size_t n = strlen(target);

            memcpy(up_path, target, n);
            memcpy(up_path + n, "/..", 4);
            err = vfs_open(up_path, O_PATH, 0, &up);
        }
    }

    kfree(up_path);

    if (err) {
        vfs_close(mp);
        return err;
    }

    struct inode *root = proc ? proc_root() : ext4_mount(dev);

    if (!root) {
        vfs_close(up);
        vfs_close(mp);
        return -EINVAL;
    }

    /* the mount point and its parent stay referenced as long as the file system is there */
    iget(dir);
    iget(up->inode);

    root->mnt_parent = up->inode;
    __atomic_store_n(&dir->mounted, root, 3); /* __ATOMIC_RELEASE */

    uint64_t fl = spin_lock_irqsave(&mounts_lock);

    if (nmounts < MOUNTS_MAX) {
        strncpy(mounts[nmounts].source, source, sizeof(mounts[0].source) - 1);
        strncpy(mounts[nmounts].target, target, sizeof(mounts[0].target) - 1);
        strncpy(mounts[nmounts].type, type, sizeof(mounts[0].type) - 1);
        mounts[nmounts].dev = root->dev;
        mounts[nmounts].dir = dir;
        mounts[nmounts].up  = up->inode;
        nmounts++;
    }

    spin_unlock_irqrestore(&mounts_lock, fl);

    vfs_close(up);
    vfs_close(mp);
    return 0;
}

/* no flags are taken but MS_RDONLY, which it is anyway */
long sys_mount(long usource, long utarget, long utype, long flags, long udata) {
    char source[64], type[16], *target;

    (void)udata;

    if (flags & ~MS_RDONLY) return -EINVAL;

    long len = strncpy_from_user(source, usource, sizeof(source) - 1);
    if (len < 0) return len;
    source[len] = 0;

    len = strncpy_from_user(type, utype, sizeof(type) - 1);
    if (len < 0) return len;
    type[len] = 0;

    int err = path_from_user(AT_FDCWD, utarget, &target);
    if (err) return err;

    err = vfs_mount(source, target, type);

    kfree(target);
    return err;
}

/* every ext4 to its disk. the ramfs and /proc are memory only */
long sys_sync(void) {
    ext4_sync_all();
    return 0;
}

/* the file system fd is on */
long sys_syncfs(long fd) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;

    long err = f->inode->ops == &ext4_ops ? ext4_sync_fs(ext4_i(f->inode)->sb) : 0;

    vfs_close(f);
    return err;
}

/* the file system whose root is at target, if it is an ext4: synced, the cache let go, and the mount point is the dir it was */
int vfs_umount(const char *target) {
    struct file *mp;

    int err = vfs_open(target, O_PATH, 0, &mp);
    if (err) return err;

    struct inode *root = mp->inode;
    int idx = -1;

    if (root->ops != &ext4_ops || !root->mnt_parent) {
        vfs_close(mp);
        return -EINVAL;
    }

    uint64_t fl = spin_lock_irqsave(&mounts_lock);

    for (int i = 0; i < nmounts; i++)
        if (mounts[i].dir->mounted == root) idx = i;

    spin_unlock_irqrestore(&mounts_lock, fl);

    if (idx < 0) {
        vfs_close(mp);
        return -EINVAL;
    }

    struct inode *dir = mounts[idx].dir, *up = mounts[idx].up;

    /* new lookups cannot get in once it is not mounted; if it turns out to be in use, it is put back */
    __atomic_store_n(&dir->mounted, NULL, 3); /* __ATOMIC_RELEASE */

    /* ours is the reference of the open above; ext4_umount() counts the rest */
    vfs_close(mp);

    err = ext4_umount(root);

    if (err) {
        __atomic_store_n(&dir->mounted, root, 3); /* __ATOMIC_RELEASE */
        return err;
    }

    fl = spin_lock_irqsave(&mounts_lock);

    for (int i = idx; i + 1 < nmounts; i++) mounts[i] = mounts[i + 1];
    nmounts--;

    spin_unlock_irqrestore(&mounts_lock, fl);

    iput(dir);
    iput(up);
    return 0;
}

long sys_umount2(long utarget, long flags) {
    char *target;

    if (flags) return -EINVAL;

    int err = path_from_user(AT_FDCWD, utarget, &target);
    if (err) return err;

    err = vfs_umount(target);

    kfree(target);
    return err;
}
