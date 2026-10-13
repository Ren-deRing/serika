#include <serika/block.h>
#include <serika/buddy.h>
#include <serika/fcntl.h>
#include <serika/mutex.h>
#include <serika/page.h>
#include <serika/printk.h>
#include <serika/string.h>
#include <serika/vfs.h>

#include <fs/ext4/ext4.h>
#include <tests/test.h>

/* the super block as it is on the disk, in two sectors */
static int read_super(struct ext4_sb *sb, struct ext4_super *out) {
    return blk_read(sb->dev, 1024 / SECTOR_SIZE, out, sizeof(*out) / SECTOR_SIZE);
}

/* a dirty block goes to the disk with the checksum made, and the first change put "not clean" on the disk first */
bool test_ext4_bflush() {
    if (!ext4_test_mount()) {
        printk("ext4: no test disk -- skipped\n");
        return true;
    }

    struct file *f;
    TEST_ASSERT(vfs_open("/mnt", O_RDONLY | O_DIRECTORY, 0, &f) == 0);

    struct inode *root = f->inode;
    struct ext4_sb *sb = ext4_i(root)->sb;
    uint64_t pblk;

    TEST_ASSERT(ext4_map(root, 0, &pblk) == 0);

    /* the buffers are bigger than a stack should have: a page */
    void *page = alloc_pages(0);
    TEST_ASSERT(page);
    uint8_t *raw = (uint8_t *)P2V((uintptr_t)page);
    struct ext4_super *es = (struct ext4_super *)(raw + 2048);

    TEST_ASSERT(read_super(sb, es) == 0);
    bool was_clean = es->state & EXT4_STATE_VALID;

    mutex_lock(&sb->meta_lock);
    TEST_ASSERT(ext4_start_changes(sb) == 0);
    TEST_ASSERT(read_super(sb, es) == 0);
    TEST_ASSERT(!(es->state & EXT4_STATE_VALID));             /* on the disk before the change */
    TEST_ASSERT(ext4_super_csum_ok(es));

    /* the root dir's block 0: its checksum spoiled in memory, and made right by the write */
    uint8_t *blk = ext4_bdirty(sb, pblk, EXT4_B_DIR, ext4_i(root)->seed);
    TEST_ASSERT(blk);
    TEST_ASSERT(ext4_dir_has_tail(blk));
    blk[EXT4_BLOCK_SIZE - 4] ^= 0xff;
    TEST_ASSERT(sb->ndirty >= 1);
    TEST_ASSERT(ext4_bflush(sb) == 0);
    TEST_ASSERT(sb->ndirty == 0);
    mutex_unlock(&sb->meta_lock);

    TEST_ASSERT(blk_read(sb->dev, pblk * (EXT4_BLOCK_SIZE / SECTOR_SIZE), raw, EXT4_BLOCK_SIZE / SECTOR_SIZE) == 0);
    TEST_ASSERT(ext4_dir_csum_ok(ext4_i(root)->seed, raw));
    TEST_ASSERT(ext4_dir_csum_ok(ext4_i(root)->seed, blk));   /* and the memory has it too */

    /* put it back as it was, if it was clean */
    if (was_clean) {
        mutex_lock(&sb->meta_lock);
        sb->es->state |= EXT4_STATE_VALID;
        TEST_ASSERT(ext4_write_super(sb) == 0);
        mutex_unlock(&sb->meta_lock);
    }

    free_pages(page, 0);
    vfs_close(f);
    return true;
}

/* a sync with something dirty writes it and makes the disk clean again; one with nothing dirty writes nothing */
bool test_ext4_sync() {
    if (!ext4_test_mount()) {
        printk("ext4: no test disk -- skipped\n");
        return true;
    }

    struct file *f;
    TEST_ASSERT(vfs_open("/mnt", O_RDONLY | O_DIRECTORY, 0, &f) == 0);

    struct inode *root = f->inode;
    struct ext4_sb *sb = ext4_i(root)->sb;
    uint64_t pblk;

    TEST_ASSERT(ext4_map(root, 0, &pblk) == 0);
    TEST_ASSERT(sb->clean_at_mount);

    void *page = alloc_pages(0);
    TEST_ASSERT(page);
    struct ext4_super *es = (struct ext4_super *)P2V((uintptr_t)page);

    TEST_ASSERT(ext4_sync_fs(sb) == 0);                       /* nothing dirty: and clean */
    TEST_ASSERT(read_super(sb, es) == 0 && (es->state & EXT4_STATE_VALID));

    mutex_lock(&sb->meta_lock);
    TEST_ASSERT(ext4_start_changes(sb) == 0);
    TEST_ASSERT(ext4_bdirty(sb, pblk, EXT4_B_DIR, ext4_i(root)->seed));
    mutex_unlock(&sb->meta_lock);

    TEST_ASSERT(read_super(sb, es) == 0 && !(es->state & EXT4_STATE_VALID));

    TEST_ASSERT(ext4_sync_fs(sb) == 0);
    TEST_ASSERT(sb->ndirty == 0);
    TEST_ASSERT(read_super(sb, es) == 0 && (es->state & EXT4_STATE_VALID) && ext4_super_csum_ok(es));

    /* a second one: nothing to write, still clean */
    TEST_ASSERT(ext4_sync_fs(sb) == 0);
    TEST_ASSERT(read_super(sb, es) == 0 && (es->state & EXT4_STATE_VALID));

    /* sync_all goes to the registry's file systems: the mounted one is there */
    mutex_lock(&sb->meta_lock);
    TEST_ASSERT(ext4_start_changes(sb) == 0);
    TEST_ASSERT(ext4_bdirty(sb, pblk, EXT4_B_DIR, ext4_i(root)->seed));
    TEST_ASSERT(sb->ndirty == 1 && !(sb->es->state & EXT4_STATE_VALID));
    mutex_unlock(&sb->meta_lock);
    TEST_ASSERT(ext4_sync_all() == 0);
    TEST_ASSERT(sb->ndirty == 0 && (sb->es->state & EXT4_STATE_VALID));

    free_pages(page, 0);
    vfs_close(f);
    return true;
}
