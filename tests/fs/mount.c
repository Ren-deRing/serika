#include <serika/errno.h>
#include <serika/fcntl.h>
#include <serika/printk.h>
#include <serika/string.h>
#include <serika/vfs.h>

#include <tests/test.h>

static bool opens(const char *path, struct inode **inode) {
    struct file *f;

    if (vfs_open(path, O_RDONLY | O_PATH, 0, &f) != 0) return false;

    if (inode) {
        iget(f->inode);
        *inode = f->inode;
    }

    vfs_close(f);
    return true;
}

/* the test disk becomes the root and the initramfs is on a dir of it, the way back up leads to the root, what was mounted
 * in the old root is still there under it; and then it is all put back as it was. the checks are made while it is pivoted and
 * looked at after, so a failure cannot leave the root where it should not be */
bool test_vfs_pivot() {
    if (!ext4_test_mount()) {
        printk("vfs: no test disk -- skipped\n");
        return true;
    }

    struct inode *initramfs = vfs_root, *up = NULL, *top = NULL, *dotdot = NULL;
    char mounts[256];

    TEST_ASSERT(vfs_mkdir("/mnt/pivot-old", 0755) == 0);

    /* what cannot be done: a dir that is not a mount, the root itself, a mount over itself, a put_old that is elsewhere */
    TEST_ASSERT(vfs_pivot_root("/mnt/pivot-old", "/mnt") == -EINVAL);
    TEST_ASSERT(vfs_pivot_root("/", "/mnt") == -EINVAL);
    TEST_ASSERT(vfs_pivot_root("/mnt", "/mnt") == -EBUSY);
    TEST_ASSERT(vfs_pivot_root("/mnt", "/tmp") == -EINVAL);
    TEST_ASSERT(vfs_root == initramfs);

    int r = vfs_pivot_root("/mnt", "/mnt/pivot-old");

    bool root_changed = vfs_root != initramfs;
    bool new_files    = opens("/many", NULL) && opens("/hello.txt", NULL);
    bool old_under    = opens("/pivot-old/bin", NULL) && opens("/pivot-old/usr/hello.txt", NULL);
    bool no_old_name  = !opens("/mnt/hello.txt", NULL);
    bool dots         = opens("/pivot-old/..", &up) && opens("/", &top) && opens("/..", &dotdot) && up == top && dotdot == top;
    size_t n          = vfs_mounts_text(mounts, sizeof(mounts) - 1, false);
    bool root_line    = n > 12 && memcmp(mounts, "sdb / ext4 rw", 13) == 0;

    mounts[n] = 0;

    if (up)     iput(up);
    if (top)    iput(top);
    if (dotdot) iput(dotdot);

    /* and back: the initramfs is the root again and the disk is on /mnt */
    int back = r == 0 ? vfs_pivot_root("/pivot-old", "/pivot-old/mnt") : -1;

    TEST_ASSERT(r == 0);
    TEST_ASSERT(back == 0);
    TEST_ASSERT(vfs_root == initramfs);

    TEST_ASSERT(root_changed && new_files && old_under && no_old_name && dots && root_line);

    TEST_ASSERT(opens("/mnt/hello.txt", NULL) && opens("/usr/hello.txt", NULL));
    n = vfs_mounts_text(mounts, sizeof(mounts) - 1, false);
    TEST_ASSERT(n > 18 && memcmp(mounts, "rootfs / rootfs rw", 18) == 0);

    TEST_ASSERT(vfs_rmdir("/mnt/pivot-old") == 0);
    return true;
}
