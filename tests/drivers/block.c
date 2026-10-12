#include <serika/block.h>
#include <serika/buddy.h>
#include <serika/page.h>
#include <serika/printk.h>
#include <serika/string.h>

#include <tests/test.h>

#define SECTORS 16

/* the disk (if there is one) reads the same in one request as in sector by sector, and has an ext4 on it */
bool test_block_read() {
    struct blkdev *dev = blk_find("vda");

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
