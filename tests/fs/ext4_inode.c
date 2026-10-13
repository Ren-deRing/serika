#include <serika/block.h>
#include <serika/buddy.h>
#include <serika/fcntl.h>
#include <serika/page.h>
#include <serika/printk.h>
#include <serika/string.h>
#include <serika/vfs.h>

#include <fs/ext4/ext4.h>
#include <tests/test.h>

/* the inode as it is on the disk: the times of a change, with the checksum right, after a sync */
bool test_ext4_inode() {
    if (!ext4_test_mount()) {
        printk("ext4: no test disk -- skipped\n");
        return true;
    }

    struct file *f;
    TEST_ASSERT(vfs_open("/mnt/hello.txt", O_RDONLY, 0, &f) == 0);

    struct inode *inode = f->inode;
    struct ext4_sb *sb = ext4_i(inode)->sb;

    /* seconds 1700000000 and 1700000001 of 2023, and the nanoseconds in the extra field */
    TEST_ASSERT(vfs_utimes(inode, 1700000000LL * 1000000000LL + 123, 1700000001LL * 1000000000LL + 456) == 0);
    TEST_ASSERT(ext4_sync_fs(sb) == 0);

    uint64_t blk;
    uint32_t off;
    TEST_ASSERT(ext4_inode_loc(sb, inode->ino, &blk, &off) == 0);

    void *page = alloc_pages(0);
    TEST_ASSERT(page);
    uint8_t *raw = (uint8_t *)P2V((uintptr_t)page);

    TEST_ASSERT(blk_read(sb->dev, blk * (EXT4_BLOCK_SIZE / SECTOR_SIZE), raw, EXT4_BLOCK_SIZE / SECTOR_SIZE) == 0);

    const struct ext4_inode *di = (const void *)(raw + off);

    TEST_ASSERT(di->atime == 1700000000U && di->mtime == 1700000001U);
    TEST_ASSERT((di->atime_extra >> 2) == 123 && (di->mtime_extra >> 2) == 456);
    TEST_ASSERT(di->links_count == 1 && di->size_lo == inode->size);
    TEST_ASSERT(ext4_inode_csum_ok(ext4_i(inode)->seed, sb->inode_size, di));
    TEST_ASSERT(ext4_i(inode)->nblocks == 8);                 /* one block of 4096, in sectors */

    free_pages(page, 0);
    vfs_close(f);
    return true;
}
