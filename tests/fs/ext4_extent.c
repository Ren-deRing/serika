#include <serika/errno.h>
#include <serika/fcntl.h>
#include <serika/mutex.h>
#include <serika/printk.h>
#include <serika/string.h>
#include <serika/vfs.h>

#include <fs/ext4/ext4.h>
#include <tests/test.h>

#define COUNT 2000 /* more than the 1360 that a root and four leaves hold: the tree has three levels */

static uint64_t pblks[COUNT];

static int depth_of(struct inode *inode) {
    return ((const struct ext4_extent_header *)ext4_i(inode)->block)->depth;
}

/* the keys of every index node equal the first entry of its child: walks the tree. *first: the first block it maps */
static bool keys_ok(struct ext4_sb *sb, const uint8_t *node, uint32_t seed, uint32_t *first) {
    const struct ext4_extent_header *h = (const void *)node;

    if (h->magic != EXT4_EXT_MAGIC) return false;
    if (!h->entries) return true;

    if (h->depth == 0) {
        *first = ((const struct ext4_extent *)(h + 1))[0].block;
        return true;
    }

    const struct ext4_extent_idx *ix = (const void *)(h + 1);

    for (unsigned i = 0; i < h->entries; i++) {
        const uint8_t *child = ext4_bread_kind(sb, (uint64_t)ix[i].leaf_hi << 32 | ix[i].leaf_lo, EXT4_B_EXTENT, seed);
        uint32_t k;

        if (!child || !keys_ok(sb, child, seed, &k) || k != ix[i].block) return false;
    }

    *first = ix[0].block;
    return true;
}

/* 2000 extents that cannot merge (a block of every two), found again, cut in the middle, and removed whole: with the
 * keys right and every block given back */
bool test_ext4_extent() {
    if (!ext4_test_mount()) {
        printk("ext4: no test disk -- skipped\n");
        return true;
    }

    struct file *f;
    TEST_ASSERT(vfs_open("/mnt/scratch", O_RDWR, 0, &f) == 0);

    struct inode *inode = f->inode;
    struct ext4_info *info = ext4_i(inode);
    struct ext4_sb *sb = info->sb;
    struct ext4_ext_found found;
    uint64_t pblk;

    mutex_lock(&inode->lock);
    mutex_lock(&sb->meta_lock);

    uint64_t before = ext4_free_blocks_count(sb);

    TEST_ASSERT(ext4_ext_next(inode, 0, &found) == 1);          /* empty */

    for (int i = 0; i < COUNT; i++) {
        uint32_t got;

        TEST_ASSERT(ext4_alloc_blocks(sb, 0, 1, &pblks[i], &got) == 0 && got == 1);
        TEST_ASSERT(ext4_ext_insert(inode, 2 * i, pblks[i], 1) == 0);
    }

    TEST_ASSERT(depth_of(inode) == 2);

    for (int i = 0; i < COUNT; i++) {
        TEST_ASSERT(ext4_map(inode, 2 * i, &pblk) == 0 && pblk == pblks[i]);
        TEST_ASSERT(ext4_map(inode, 2 * i + 1, &pblk) == 1);   /* the hole between */
    }

    uint32_t k = 0;
    TEST_ASSERT(keys_ok(sb, (const uint8_t *)info->block, info->seed, &k));

    /* the first half goes: the keys above move on with the first entry */
    TEST_ASSERT(ext4_ext_remove(inode, 0, COUNT) == 0);
    TEST_ASSERT(ext4_ext_next(inode, 0, &found) == 0 && found.lblk == COUNT);
    TEST_ASSERT(keys_ok(sb, (const uint8_t *)info->block, info->seed, &k) && k == COUNT);
    TEST_ASSERT(ext4_map(inode, 2 * (COUNT / 2 + 5), &pblk) == 0 && pblk == pblks[COUNT / 2 + 5]);

    /* and the rest, then nothing is left of the tree and the blocks are all back */
    TEST_ASSERT(ext4_ext_remove(inode, 0, 1ULL << 32) == 0);
    TEST_ASSERT(ext4_ext_next(inode, 0, &found) == 1);
    TEST_ASSERT(depth_of(inode) == 0);
    TEST_ASSERT(info->nblocks == 0);
    TEST_ASSERT(ext4_free_blocks_count(sb) == before);

    /* an extent of 100 blocks cut in the middle is two, and a hole filled with blocks in a row joins the one before it */
    uint64_t start, s2;
    uint32_t got, g2;

    TEST_ASSERT(ext4_alloc_blocks(sb, 0, 100, &start, &got) == 0 && got == 100);
    TEST_ASSERT(ext4_ext_insert(inode, 5000, start, 100) == 0);
    TEST_ASSERT(ext4_ext_remove(inode, 5040, 5060) == 0);
    TEST_ASSERT(ext4_ext_next(inode, 5000, &found) == 0 && found.lblk == 5000 && found.len == 40 && found.pblk == start);
    TEST_ASSERT(ext4_ext_next(inode, 5040, &found) == 0 && found.lblk == 5060 && found.len == 40 && found.pblk == start + 60);
    TEST_ASSERT(info->nblocks == 80 * 8);

    TEST_ASSERT(ext4_alloc_blocks(sb, start + 40, 20, &s2, &g2) == 0 && s2 == start + 40 && g2 == 20);
    TEST_ASSERT(ext4_ext_insert(inode, 5040, s2, 20) == 0);
    TEST_ASSERT(ext4_ext_next(inode, 5000, &found) == 0 && found.len == 60);   /* it joined the one before it */
    TEST_ASSERT(ext4_ext_next(inode, 5060, &found) == 0 && found.lblk == 5060 && found.len == 40);

    TEST_ASSERT(ext4_ext_remove(inode, 0, 1ULL << 32) == 0);
    TEST_ASSERT(info->nblocks == 0 && ext4_free_blocks_count(sb) == before);

    ext4_mark_dirty(inode);
    mutex_unlock(&sb->meta_lock);
    mutex_unlock(&inode->lock);

    TEST_ASSERT(ext4_sync_fs(sb) == 0);
    vfs_close(f);
    return true;
}
