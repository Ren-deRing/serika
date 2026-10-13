#include <serika/block.h>
#include <serika/buddy.h>
#include <serika/page.h>
#include <serika/printk.h>
#include <serika/string.h>

#include <tests/test.h>

#define SECTORS 16

/* the disk (if there is one) reads the same in one request as in sector by sector, and has an ext4 on it */
bool test_block_read() {
    struct blkdev *dev = blk_find("sda");

    if (!dev) {
        printk("block: no disk -- skipped\n");
        return true;
    }

    /* the direct map: a buffer in the kernel image has no physical address that way */
    void *a = alloc_pages(1), *b = alloc_pages(1);
    TEST_ASSERT(a && b);

    uint8_t *whole = (uint8_t *)P2V((uintptr_t)a), *piece = (uint8_t *)P2V((uintptr_t)b);

    TEST_ASSERT(blk_read(dev, 0, whole, SECTORS) == 0);

    for (int i = 0; i < SECTORS; i++) TEST_ASSERT(blk_read(dev, i, piece + i * SECTOR_SIZE, 1) == 0);

    TEST_ASSERT(memcmp(whole, piece, SECTORS * SECTOR_SIZE) == 0);

    /* the superblock is at 1024, its magic at +56 */
    TEST_ASSERT(whole[1024 + 56] == 0x53 && whole[1024 + 57] == 0xef);

    /* past the end */
    TEST_ASSERT(blk_read(dev, dev->sectors, whole, 1) != 0);

    free_pages(a, 1);
    free_pages(b, 1);
    return true;
}

/* a sector written reads back, and the disk is as it was after: the pattern goes over a sector that is put back */
bool test_block_write() {
    struct blkdev *dev = blk_find("sda");

    if (!dev || !dev->write) {
        printk("block: no disk to write -- skipped\n");
        return true;
    }

    void *a = alloc_pages(0), *b = alloc_pages(0);
    TEST_ASSERT(a && b);

    uint8_t *saved = (uint8_t *)P2V((uintptr_t)a), *buf = (uint8_t *)P2V((uintptr_t)b);

    /* the last sector of the disk, and the two before it, as one write of 2 sectors and one of 1 */
    uint64_t at = dev->sectors - 3;

    TEST_ASSERT(blk_read(dev, at, saved, 3) == 0);

    for (int i = 0; i < 3 * SECTOR_SIZE; i++) buf[i] = (uint8_t)(i * 7 + 1);

    TEST_ASSERT(blk_write(dev, at, buf, 2) == 0);
    TEST_ASSERT(blk_write(dev, at + 2, buf + 2 * SECTOR_SIZE, 1) == 0);
    TEST_ASSERT(blk_flush(dev) == 0);

    memset(buf, 0, 3 * SECTOR_SIZE);
    TEST_ASSERT(blk_read(dev, at, buf, 3) == 0);

    for (int i = 0; i < 3 * SECTOR_SIZE; i++) TEST_ASSERT(buf[i] == (uint8_t)(i * 7 + 1));

    TEST_ASSERT(blk_write(dev, at, saved, 3) == 0);
    TEST_ASSERT(blk_flush(dev) == 0);

    TEST_ASSERT(blk_read(dev, at, buf, 3) == 0);
    TEST_ASSERT(memcmp(buf, saved, 3 * SECTOR_SIZE) == 0);

    /* past the end */
    TEST_ASSERT(blk_write(dev, dev->sectors, buf, 1) != 0);

    free_pages(a, 0);
    free_pages(b, 0);
    return true;
}
