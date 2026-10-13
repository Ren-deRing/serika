#include <serika/block.h>
#include <serika/errno.h>
#include <serika/fcntl.h>
#include <serika/mutex.h>
#include <serika/printk.h>
#include <serika/string.h>
#include <serika/vfs.h>

#include <fs/ext4/ext4.h>
#include <tests/test.h>

#define PAGE 4096

static uint8_t buf[3 * PAGE], back[3 * PAGE];

static long wr(struct file *f, const void *p, size_t n, uint64_t at) {
    uint64_t pos = at;
    return kernel_write(f, p, n, &pos);
}

static long rd(struct file *f, void *p, size_t n, uint64_t at) {
    uint64_t pos = at;
    return kernel_read(f, p, n, &pos);
}

static void fill(uint8_t *p, size_t n, uint8_t seed) {
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(seed + i * 7);
}

/* the byte at x of /mnt/big, which was written in pieces of 3 pages from page 0, each filled with fill(.., first page) */
static uint8_t expect_big(uint64_t x) {
    uint64_t base_off = x / (3 * PAGE) * (3 * PAGE);

    return (uint8_t)(base_off / PAGE + (x - base_off) * 7);
}

/* a file written over, appended to, with a hole, cut down and grown, and cut in the middle */
bool test_ext4_write() {
    if (!ext4_test_mount()) {
        printk("ext4: no test disk -- skipped\n");
        return true;
    }

    struct file *f;
    TEST_ASSERT(vfs_open("/mnt/hello.txt", O_RDWR, 0, &f) == 0);

    struct ext4_sb *sb = ext4_i(f->inode)->sb;
    struct ext4_info *info = ext4_i(f->inode);

    mutex_lock(&sb->meta_lock);
    uint64_t free0 = ext4_free_blocks_count(sb);
    mutex_unlock(&sb->meta_lock);

    /* over the beginning: the rest of the line is what it was */
    TEST_ASSERT(wr(f, "HELLO", 5, 0) == 5);
    TEST_ASSERT(rd(f, back, 25, 0) == 25 && memcmp(back, "HELLO from the test disk\n", 25) == 0);

    /* two pages after a hole of one and more: the size grows, the hole reads as zeros */
    fill(buf, 2 * PAGE, 1);
    TEST_ASSERT(wr(f, buf, 2 * PAGE, 2 * PAGE + 100) == 2 * PAGE);
    TEST_ASSERT(f->inode->size == 4 * PAGE + 100);
    TEST_ASSERT(rd(f, back, 2 * PAGE, 2 * PAGE + 100) == 2 * PAGE && memcmp(back, buf, 2 * PAGE) == 0);
    TEST_ASSERT(rd(f, back, PAGE, PAGE) == PAGE);
    for (int i = 0; i < PAGE; i++) TEST_ASSERT(back[i] == 0);
    TEST_ASSERT(info->nblocks == 4 * 8);                       /* the first block and three more */

    /* cut down in the middle of a page and grown again: what was past the cut does not come back */
    TEST_ASSERT(vfs_truncate(f->inode, 2 * PAGE + 150) == 0);
    TEST_ASSERT(vfs_truncate(f->inode, 4 * PAGE) == 0);
    TEST_ASSERT(rd(f, back, 2 * PAGE - 150, 2 * PAGE + 150) == 2 * PAGE - 150);
    for (int i = 0; i < 2 * PAGE - 150; i++) TEST_ASSERT(back[i] == 0);
    TEST_ASSERT(rd(f, back, 50, 2 * PAGE + 100) == 50 && memcmp(back, buf, 50) == 0);
    TEST_ASSERT(info->nblocks == 2 * 8);                       /* the first block and the one with the cut */

    /* an offset that a file cannot have */
    TEST_ASSERT(wr(f, buf, 10, 1ULL << 44) == -EFBIG);

    /* to nothing: every block back */
    TEST_ASSERT(vfs_truncate(f->inode, 0) == 0);
    TEST_ASSERT(info->nblocks == 0);
    mutex_lock(&sb->meta_lock);
    TEST_ASSERT(ext4_free_blocks_count(sb) == free0 + 1);
    mutex_unlock(&sb->meta_lock);
    vfs_close(f);

    /* a file of 64 pages in pieces of three, and a hole made of 10 of them */
    TEST_ASSERT(vfs_open("/mnt/big", O_RDWR, 0, &f) == 0);

    for (int p = 0; p < 64; p += 3) {
        int n = p + 3 <= 64 ? 3 : 1;

        fill(buf, n * PAGE, p);
        TEST_ASSERT(wr(f, buf, n * PAGE, (uint64_t)p * PAGE) == n * PAGE);
    }

    TEST_ASSERT(f->inode->size == 64 * PAGE && ext4_i(f->inode)->nblocks == 64 * 8);
    TEST_ASSERT(vfs_punch_hole(f->inode, 10 * PAGE, 10 * PAGE) == 0);
    TEST_ASSERT(f->inode->size == 64 * PAGE && ext4_i(f->inode)->nblocks == 54 * 8);

    TEST_ASSERT(rd(f, back, PAGE, 12 * PAGE) == PAGE);
    for (int i = 0; i < PAGE; i++) TEST_ASSERT(back[i] == 0);

    TEST_ASSERT(rd(f, back, PAGE, 21 * PAGE) == PAGE);
    for (int i = 0; i < PAGE; i++) TEST_ASSERT(back[i] == expect_big(21 * PAGE + i));

    /* a part of a page: the bytes around it stay */
    TEST_ASSERT(vfs_punch_hole(f->inode, 30 * PAGE + 100, 200) == 0);
    TEST_ASSERT(rd(f, back, 300, 30 * PAGE + 50) == 300);
    for (int i = 0; i < 300; i++) {
        uint64_t x = 30 * PAGE + 50 + i;

        TEST_ASSERT(back[i] == (i >= 50 && i < 250 ? 0 : expect_big(x)));
    }

    vfs_close(f);
    return true;
}

/* an unwritten extent is not written over; no space is a short write and then the error; and all of it is still there
 * after the file system is unmounted and mounted again */
bool test_ext4_persist() {
    if (!ext4_test_mount()) {
        printk("ext4: no test disk -- skipped\n");
        return true;
    }

    struct file *f;
    struct ext4_sb *sb;

    TEST_ASSERT(vfs_open("/mnt/scratch", O_RDWR, 0, &f) == 0);
    sb = ext4_i(f->inode)->sb;
    fill(buf, PAGE, 3);
    TEST_ASSERT(wr(f, buf, PAGE, 0) == PAGE);

    /* unwritten: the extent of scratch is marked so, in memory */
    struct ext4_extent_header *h = (struct ext4_extent_header *)ext4_i(f->inode)->block;
    struct ext4_extent *e = (struct ext4_extent *)(h + 1);

    TEST_ASSERT(h->depth == 0 && h->entries == 1 && e[0].len == 1);
    e[0].len += 32768;
    TEST_ASSERT(wr(f, buf, 10, 0) == -EOPNOTSUPP);
    e[0].len -= 32768;
    TEST_ASSERT(wr(f, buf, 10, 0) == 10);

    /* no space: all of the free blocks taken, the write to a hole gets -ENOSPC */
    static uint64_t start[64];
    static uint32_t len[64];
    int n = 0;

    mutex_lock(&sb->meta_lock);
    uint64_t free0 = ext4_free_blocks_count(sb);
    while (n < 64 && ext4_alloc_blocks(sb, 0, 32768, &start[n], &len[n]) == 0) n++;
    mutex_unlock(&sb->meta_lock);

    TEST_ASSERT(n < 64);
    TEST_ASSERT(wr(f, buf, PAGE, 100 * PAGE) == -ENOSPC);
    TEST_ASSERT(f->inode->size == PAGE);

    mutex_lock(&sb->meta_lock);
    for (int i = 0; i < n; i++) TEST_ASSERT(ext4_free_blocks(sb, start[i], len[i]) == 0);
    TEST_ASSERT(ext4_free_blocks_count(sb) == free0);
    mutex_unlock(&sb->meta_lock);

    vfs_close(f);

    /* what is on the disk: unmounted, mounted again, read */
    TEST_ASSERT(ext4_sync_fs(sb) == 0);
    TEST_ASSERT(vfs_umount("/mnt") == 0);
    TEST_ASSERT(ext4_test_mount());

    TEST_ASSERT(vfs_open("/mnt/scratch", O_RDONLY, 0, &f) == 0);
    TEST_ASSERT(f->inode->size == PAGE);
    TEST_ASSERT(rd(f, back, PAGE, 0) == PAGE && memcmp(back, buf, PAGE) == 0);
    vfs_close(f);

    TEST_ASSERT(vfs_open("/mnt/big", O_RDONLY, 0, &f) == 0);
    TEST_ASSERT(f->inode->size == 64 * PAGE);
    TEST_ASSERT(rd(f, back, PAGE, 12 * PAGE) == PAGE);
    for (int i = 0; i < PAGE; i++) TEST_ASSERT(back[i] == 0);
    TEST_ASSERT(rd(f, back, PAGE, 21 * PAGE) == PAGE);
    for (int i = 0; i < PAGE; i++) TEST_ASSERT(back[i] == expect_big(21 * PAGE + i));
    vfs_close(f);
    return true;
}
