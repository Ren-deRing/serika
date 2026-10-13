#include <serika/buddy.h>
#include <serika/errno.h>
#include <serika/mm.h>
#include <serika/page.h>
#include <serika/printk.h>
#include <serika/slab.h>
#include <serika/string.h>

#include "ext4.h"

/* one block of the cache. the page stays: nobody frees a block, and what is given out is a plain pointer.
 * TODO: let go of blocks when memory is short */
struct bcache_entry {
    list_node  node;
    list_node  dirty;       /* dirty_list, when is_dirty */
    uint64_t   blk;
    void      *data;
    bool       is_dirty;
    uint8_t    kind;        /* what the checksum of the block is made from when it is written */
    uint32_t   seed;
};

static int disk_read(struct ext4_sb *sb, uint64_t blk, void *data) {
    return blk_read(sb->dev, blk * (EXT4_BLOCK_SIZE / SECTOR_SIZE), data, EXT4_BLOCK_SIZE / SECTOR_SIZE);
}

/* a block that came from the disk is whole if its checksum is: only with metadata_csum, only for the kinds that have one */
static bool block_ok(struct ext4_sb *sb, uint64_t blk, enum ext4_bkind kind, uint32_t seed, const void *data) {
    if (!sb->csum) return true;

    bool ok = true;

    if (kind == EXT4_B_EXTENT)   ok = ext4_extent_csum_ok(seed, data);
    else if (kind == EXT4_B_DIR) ok = ext4_dir_csum_ok(seed, data);

    if (!ok) printk("ext4: %s: block %lu: %s checksum\n", sb->dev->name, blk, kind == EXT4_B_EXTENT ? "extent" : "directory");
    return ok;
}

/* the entry of blk, from the cache or made: read from the disk (and checked), or not read at all. bcache_lock held */
static struct bcache_entry *bget(struct ext4_sb *sb, uint64_t blk, enum ext4_bkind kind, uint32_t seed, bool read) {
    if (blk >= sb->blocks_count) return NULL;

    list_node *head = &sb->bcache[blk % EXT4_BCACHE_BUCKETS];

    for (list_node *n = head->next; n != head; n = n->next) {
        struct bcache_entry *e = container_of(n, struct bcache_entry, node);

        if (e->blk == blk) return e; /* it was whole when it came, and what changes it makes whole again */
    }

    struct bcache_entry *e = kmalloc(sizeof(*e));
    void *page = alloc_pages(0);

    if (!e || !page) {
        kfree(e);
        if (page) free_pages(page, 0);
        return NULL;
    }

    e->blk      = blk;
    e->data     = (void *)P2V((uintptr_t)page);
    e->is_dirty = false;
    e->kind     = EXT4_B_RAW;
    e->seed     = 0;
    list_init(&e->dirty);

    if (read && (disk_read(sb, blk, e->data) != 0 || !block_ok(sb, blk, kind, seed, e->data))) {
        free_pages(page, 0);
        kfree(e);
        return NULL;
    }

    list_add_tail(&e->node, head);
    return e;
}

const void *ext4_bread_kind(struct ext4_sb *sb, uint64_t blk, enum ext4_bkind kind, uint32_t seed) {
    mutex_lock(&sb->bcache_lock);

    struct bcache_entry *e = bget(sb, blk, kind, seed, true);
    const void *data = e ? e->data : NULL;

    mutex_unlock(&sb->bcache_lock);
    return data;
}

const void *ext4_bread(struct ext4_sb *sb, uint64_t blk) {
    return ext4_bread_kind(sb, blk, EXT4_B_RAW, 0);
}

static void mark_dirty(struct ext4_sb *sb, struct bcache_entry *e, enum ext4_bkind kind, uint32_t seed) {
    e->kind = kind;
    e->seed = seed;

    if (!e->is_dirty) {
        e->is_dirty = true;
        list_add_tail(&e->dirty, &sb->dirty_list);
        sb->ndirty++;
    }
}

void *ext4_bdirty(struct ext4_sb *sb, uint64_t blk, enum ext4_bkind kind, uint32_t seed) {
    mutex_lock(&sb->bcache_lock);

    struct bcache_entry *e = bget(sb, blk, kind, seed, true);

    if (e) mark_dirty(sb, e, kind, seed);

    void *data = e ? e->data : NULL;

    mutex_unlock(&sb->bcache_lock);
    return data;
}

void *ext4_bnew(struct ext4_sb *sb, uint64_t blk, enum ext4_bkind kind, uint32_t seed) {
    mutex_lock(&sb->bcache_lock);

    struct bcache_entry *e = bget(sb, blk, kind, seed, false);

    if (e) {
        memset(e->data, 0, EXT4_BLOCK_SIZE);
        mark_dirty(sb, e, kind, seed);
    }

    void *data = e ? e->data : NULL;

    mutex_unlock(&sb->bcache_lock);
    return data;
}

/* the checksum of a block that is going out */
static void block_csum_set(struct ext4_sb *sb, struct bcache_entry *e) {
    if (!sb->csum) return;

    if (e->kind == EXT4_B_EXTENT)   ext4_extent_csum_set(e->seed, e->data);
    else if (e->kind == EXT4_B_DIR) ext4_dir_csum_set(e->seed, e->data);
}

int ext4_bflush(struct ext4_sb *sb) {
    int err = 0;

    mutex_lock(&sb->bcache_lock);

    while (!list_empty(&sb->dirty_list)) {
        struct bcache_entry *e = container_of(sb->dirty_list.next, struct bcache_entry, dirty);

        block_csum_set(sb, e);

        err = blk_write(sb->dev, e->blk * (EXT4_BLOCK_SIZE / SECTOR_SIZE), e->data, EXT4_BLOCK_SIZE / SECTOR_SIZE);

        if (err) {
            printk("ext4: %s: write of block %lu failed: read only now\n", sb->dev->name, e->blk);
            sb->readonly = true;
            break;
        }

        list_del(&e->dirty);
        list_init(&e->dirty);
        e->is_dirty = false;
        sb->ndirty--;
    }

    mutex_unlock(&sb->bcache_lock);
    return err;
}

void ext4_bfree(struct ext4_sb *sb) {
    for (int b = 0; b < EXT4_BCACHE_BUCKETS; b++) {
        list_node *head = &sb->bcache[b];

        while (head->next != head) {
            struct bcache_entry *e = container_of(head->next, struct bcache_entry, node);

            list_del(&e->node);
            free_pages((void *)V2P((uintptr_t)e->data), 0);
            kfree(e);
        }
    }
}
