#include <serika/block.h>
#include <serika/buddy.h>
#include <serika/errno.h>
#include <serika/fcntl.h>
#include <serika/page.h>
#include <serika/printk.h>
#include <serika/string.h>
#include <serika/vfs.h>

#include <fs/ext4/ext4.h>
#include <tests/test.h>

/* the superblock is at 1024: the checksum is its last 4 bytes, in the second of its two sectors */
#define CSUM_SECTOR    3
#define CSUM_IN_SECTOR (0x3fc - SECTOR_SIZE)

/* the test disk (sdb, checksums on) mounted on /mnt: once. false: there is no such disk */
bool ext4_test_mount(void) {
    struct file *f;

    if (vfs_open("/mnt/hello.txt", O_RDONLY, 0, &f) == 0) {
        vfs_close(f);
        return true;
    }

    if (!blk_find("sdb")) return false;

    vfs_mkdir("/mnt", 0755); /* it may be there */
    return vfs_mount("sdb", "/mnt", "ext4") == 0;
}

/* a super block whose checksum is wrong is not mounted; the right one is */
bool test_ext4_mount_csum() {
    struct blkdev *dev = blk_find("sdb");

    if (!dev) {
        printk("ext4: no test disk -- skipped\n");
        return true;
    }

    /* only before it is mounted: after, the cache has the good one and the disk is not read again */
    struct file *f;
    if (vfs_open("/mnt/hello.txt", O_RDONLY, 0, &f) == 0) {
        vfs_close(f);
        printk("ext4: test disk already mounted -- skipped\n");
        return true;
    }

    void *page = alloc_pages(0);
    TEST_ASSERT(page);
    uint8_t *sec = (uint8_t *)P2V((uintptr_t)page);

    TEST_ASSERT(blk_read(dev, CSUM_SECTOR, sec, 1) == 0);
    sec[CSUM_IN_SECTOR] ^= 0x01;
    TEST_ASSERT(blk_write(dev, CSUM_SECTOR, sec, 1) == 0);

    vfs_mkdir("/mnt", 0755);
    TEST_ASSERT(vfs_mount("sdb", "/mnt", "ext4") != 0);

    sec[CSUM_IN_SECTOR] ^= 0x01;
    TEST_ASSERT(blk_write(dev, CSUM_SECTOR, sec, 1) == 0);
    free_pages(page, 0);

    TEST_ASSERT(vfs_mount("sdb", "/mnt", "ext4") == 0);
    return true;
}

/* the test disk reads: a file, a directory of several blocks (indexed), with every checksum checked on the way */
bool test_ext4_read_csum() {
    if (!ext4_test_mount()) {
        printk("ext4: no test disk -- skipped\n");
        return true;
    }

    struct file *f;
    char buf[64];
    uint64_t pos = 0;

    static const char text[] = "hello from the test disk\n";

    TEST_ASSERT(vfs_open("/mnt/hello.txt", O_RDONLY, 0, &f) == 0);
    TEST_ASSERT(ext4_i(f->inode)->sb->csum);
    TEST_ASSERT(!ext4_i(f->inode)->sb->readonly);
    TEST_ASSERT(kernel_read(f, buf, sizeof(buf), &pos) == sizeof(text) - 1);
    TEST_ASSERT(memcmp(buf, text, sizeof(text) - 1) == 0);
    vfs_close(f);

    /* the 300 names: the first, one in the middle, the last, and one that is not there */
    static const char *names[] = { "0001", "0150", "0300" };
    char path[96];

    for (int i = 0; i < 3; i++) {
        ksnprintf(path, sizeof(path), "/mnt/many/an-entry-with-a-long-name-%s", names[i]);
        TEST_ASSERT(vfs_open(path, O_RDONLY, 0, &f) == 0);
        pos = 0;
        TEST_ASSERT(kernel_read(f, buf, sizeof(buf), &pos) > 0);
        vfs_close(f);
    }

    TEST_ASSERT(vfs_open("/mnt/many/an-entry-with-a-long-name-0301", O_RDONLY, 0, &f) == -ENOENT);

    /* the dir is more than a block, so it is indexed: its nodes have no tail and are not an error */
    TEST_ASSERT(vfs_open("/mnt/many", O_RDONLY | O_DIRECTORY, 0, &f) == 0);
    TEST_ASSERT(f->inode->size > EXT4_BLOCK_SIZE);
    vfs_close(f);

    /* /usr has no checksums, and reads as before */
    if (vfs_open("/usr/hello.txt", O_RDONLY, 0, &f) == 0) {
        TEST_ASSERT(!ext4_i(f->inode)->sb->csum);
        vfs_close(f);
    }

    return true;
}

/* a mounted fs with a file open does not unmount; with none open it does, and mounts again */
bool test_ext4_umount() {
    if (!ext4_test_mount()) {
        printk("ext4: no test disk -- skipped\n");
        return true;
    }

    struct file *f, *g;
    char buf[32];
    uint64_t pos = 0;

    TEST_ASSERT(vfs_open("/mnt/hello.txt", O_RDONLY, 0, &f) == 0);
    TEST_ASSERT(vfs_umount("/mnt") == -EBUSY);
    TEST_ASSERT(kernel_read(f, buf, 5, &pos) == 5);           /* and it works */
    vfs_close(f);

    TEST_ASSERT(vfs_open("/mnt/many/an-entry-with-a-long-name-0100", O_RDONLY, 0, &f) == 0);
    vfs_close(f);

    TEST_ASSERT(vfs_umount("/mnt") == 0);
    TEST_ASSERT(vfs_open("/mnt/hello.txt", O_RDONLY, 0, &g) == -ENOENT);
    TEST_ASSERT(vfs_umount("/mnt") == -EINVAL);               /* not a mount any more */
    TEST_ASSERT(vfs_umount("/usr/bin") == -EINVAL);           /* not the root of a fs */

    TEST_ASSERT(ext4_test_mount());                           /* again: the same disk, the same files */
    pos = 0;
    TEST_ASSERT(vfs_open("/mnt/hello.txt", O_RDONLY, 0, &f) == 0);
    TEST_ASSERT(kernel_read(f, buf, 5, &pos) == 5 && memcmp(buf, "hello", 5) == 0);
    vfs_close(f);
    return true;
}
