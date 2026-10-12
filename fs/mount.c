#include <serika/block.h>
#include <serika/errno.h>
#include <serika/fcntl.h>
#include <serika/mm.h>
#include <serika/printk.h>
#include <serika/slab.h>
#include <serika/stat.h>
#include <serika/string.h>
#include <serika/syscall.h>
#include <serika/vfs.h>

#include "ext4/ext4.h"

#define MS_RDONLY 1

/* target becomes the root of the file system on source. it stays mounted: no umount yet */
int vfs_mount(const char *source, const char *target, const char *type) {
    if (strcmp(type, "ext4") != 0) return -ENODEV;

    if (strncmp(source, "/dev/", 5) == 0) source += 5;

    struct blkdev *dev = blk_find(source);
    if (!dev) return -ENOENT;

    struct file *mp, *up;

    int err = vfs_open(target, O_PATH, 0, &mp);
    if (err) return err;

    struct inode *dir = mp->inode;

    if (!S_ISDIR(dir->mode)) err = -ENOTDIR;
    else if (dir == vfs_root || dir->mounted || dir->mnt_parent) err = -EBUSY;

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

    struct inode *root = ext4_mount(dev);

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
