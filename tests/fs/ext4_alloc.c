#include <serika/block.h>
#include <serika/buddy.h>
#include <serika/errno.h>
#include <serika/fcntl.h>
#include <serika/mutex.h>
#include <serika/page.h>
#include <serika/printk.h>
#include <serika/string.h>
#include <serika/vfs.h>

#include <fs/ext4/ext4.h>
#include <tests/test.h>

#define RANGES 64

/* blocks come out of a group and go back, the counters follow, the descriptor and the bitmap on the disk have their
 * checksums; and when there are no more it is -ENOSPC and giving them all back leaves what there was */
bool test_ext4_alloc() {
    if (!ext4_test_mount()) {
        printk("ext4: no test disk -- skipped\n");
        return true;
    }

    struct file *f;
    TEST_ASSERT(vfs_open("/mnt", O_RDONLY | O_DIRECTORY, 0, &f) == 0);

    struct ext4_sb *sb = ext4_i(f->inode)->sb;
    static uint64_t start[RANGES];
    static uint32_t len[RANGES];
    int n = 0;

    mutex_lock(&sb->meta_lock);

    uint64_t before = ext4_free_blocks_count(sb);

    /* some blocks, in a row */
    TEST_ASSERT(ext4_alloc_blocks(sb, 0, 100, &start[0], &len[0]) == 0);
    TEST_ASSERT(len[0] >= 1 && len[0] <= 100);
    TEST_ASSERT(ext4_free_blocks_count(sb) == before - len[0]);
    n = 1;

    /* the next ones follow, if the goal says so */
    TEST_ASSERT(ext4_alloc_blocks(sb, start[0] + len[0], 1, &start[1], &len[1]) == 0);
    TEST_ASSERT(len[1] == 1 && start[1] == start[0] + len[0]);
    n = 2;

    /* it is on the disk after a sync: the bitmap has the bits, and the checksums are right */
    mutex_unlock(&sb->meta_lock);
    TEST_ASSERT(ext4_sync_fs(sb) == 0);

    void *gpage = alloc_pages(0), *bpage = alloc_pages(0);
    TEST_ASSERT(gpage && bpage);
    uint8_t *graw = (uint8_t *)P2V((uintptr_t)gpage), *braw = (uint8_t *)P2V((uintptr_t)bpage);

    /* group 0's descriptor is the first in the table (block 1) */
    TEST_ASSERT(blk_read(sb->dev, 1 * (EXT4_BLOCK_SIZE / SECTOR_SIZE), graw, EXT4_BLOCK_SIZE / SECTOR_SIZE) == 0);

    const struct ext4_group_desc *gd = (const void *)graw;
    uint64_t bitmap = gd->block_bitmap_lo | (uint64_t)gd->block_bitmap_hi << 32;

    TEST_ASSERT(ext4_gd_csum(sb->csum_seed, sb->desc_size, 0, gd) == gd->checksum);

    TEST_ASSERT(blk_read(sb->dev, bitmap * (EXT4_BLOCK_SIZE / SECTOR_SIZE), braw, EXT4_BLOCK_SIZE / SECTOR_SIZE) == 0);
    TEST_ASSERT(braw[start[0] >> 3] & (1U << (start[0] & 7)));
    TEST_ASSERT((gd->block_bitmap_csum_lo | (uint32_t)gd->block_bitmap_csum_hi << 16) ==
                ext4_crc32c(sb->csum_seed, braw, sb->blocks_per_group / 8));

    free_pages(bpage, 0);
    free_pages(gpage, 0);

    /* all of what is left, then no more */
    mutex_lock(&sb->meta_lock);

    int err = 0;

    while (n < RANGES) {
        err = ext4_alloc_blocks(sb, 0, 32768, &start[n], &len[n]);
        if (err) break;
        n++;
    }

    TEST_ASSERT(err == -ENOSPC);                              /* the groups that are uninit are not used, so this is soon */
    TEST_ASSERT(ext4_free_blocks_count(sb) < before);

    for (int i = 0; i < n; i++) TEST_ASSERT(ext4_free_blocks(sb, start[i], len[i]) == 0);

    TEST_ASSERT(ext4_free_blocks_count(sb) == before);

    /* a range outside the disk is refused */
    TEST_ASSERT(ext4_free_blocks(sb, sb->blocks_count, 1) == -EINVAL);

    mutex_unlock(&sb->meta_lock);

    TEST_ASSERT(ext4_sync_fs(sb) == 0);
    vfs_close(f);
    return true;
}
