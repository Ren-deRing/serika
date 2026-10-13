#include <serika/buddy.h>
#include <serika/errno.h>
#include <serika/mm.h>
#include <serika/page.h>
#include <serika/printk.h>
#include <serika/string.h>

#include "ext4.h"

/* the pages of a file that are in the cache: a radix tree of nodes of 512 slots, a page each, that grows in height with
 * the file (1 level: 512 pages, 2: 1 GiB of them, 3: 512 GiB, 4: all that a file can have). a slot of a leaf holds the
 * frame of the page and its dirty bit; the other slots hold the node below. a node that was not needed does not exist, so
 * a hole costs nothing. everything is done with the inode lock held */

#define TREE_BITS  9
#define TREE_SLOTS (1U << TREE_BITS)

static inline uintptr_t *node_slots(uintptr_t node) {
    return (uintptr_t *)P2V(node);
}

static uintptr_t node_new(void) {
    uintptr_t phys = (uintptr_t)alloc_pages(0);

    if (phys) memset((void *)P2V(phys), 0, PAGE_SIZE);

    return phys;
}

/* the slot of the page at index; made, with the nodes down to it, when create. else NULL if there is none */
static uintptr_t *page_slot(struct ext4_info *info, uint64_t index, bool create) {
    while (info->height == 0 || (index >> (TREE_BITS * info->height)) != 0) {
        if (!create) return NULL;

        uintptr_t root = node_new();
        if (!root) return NULL;

        if (info->height) node_slots(root)[0] = info->root;

        info->root = root;
        info->height++;
    }

    uintptr_t node = info->root;

    for (unsigned level = info->height; level > 1; level--) {
        uintptr_t *slot = &node_slots(node)[(index >> (TREE_BITS * (level - 1))) & (TREE_SLOTS - 1)];

        if (!*slot) {
            if (!create) return NULL;

            *slot = node_new();
            if (!*slot) return NULL;
        }

        node = *slot;
    }

    return &node_slots(node)[index & (TREE_SLOTS - 1)];
}

typedef int (*walk_fn)(struct inode *inode, uint64_t index, uintptr_t *slot, void *arg);

/* the pages in [from, to) that are in the cache, in order. fn is not 0: stop, with that */
static int walk_node(struct inode *inode, uintptr_t node, unsigned level, uint64_t base, uint64_t from, uint64_t to,
                     walk_fn fn, void *arg) {
    uintptr_t *slots = node_slots(node);
    uint64_t span = 1ULL << (TREE_BITS * (level - 1));

    for (unsigned i = 0; i < TREE_SLOTS; i++) {
        uint64_t lo = base + i * span, hi = lo + span;

        if (hi <= from) continue;
        if (lo >= to) break;
        if (!slots[i]) continue;

        int r = level == 1 ? fn(inode, lo, &slots[i], arg) : walk_node(inode, slots[i], level - 1, lo, from, to, fn, arg);

        if (r) return r;
    }

    return 0;
}

static int walk(struct inode *inode, uint64_t from, uint64_t to, walk_fn fn, void *arg) {
    struct ext4_info *info = ext4_i(inode);

    if (!info->height) return 0;

    return walk_node(inode, info->root, info->height, 0, from, to, fn, arg);
}

uintptr_t ext4_page_get(struct inode *inode, uint64_t index) {
    uintptr_t *slot = page_slot(ext4_i(inode), index, false);

    return slot ? PG(*slot) : 0;
}

/* a page from the disk (a hole is zeros), into the slot */
static int page_load(struct inode *inode, uint64_t index, uintptr_t *slot) {
    struct ext4_info *info = ext4_i(inode);
    uint64_t pblk;

    int err = ext4_map(inode, index, &pblk);
    if (err < 0) return err;

    uintptr_t phys = (uintptr_t)alloc_pages(0);
    if (!phys) return -ENOMEM;

    void *data = (void *)P2V(phys);

    if (err > 0) {
        memset(data, 0, PAGE_SIZE); /* a hole */
    } else if (pblk >= info->sb->blocks_count ||
               blk_read(info->sb->dev, pblk * (EXT4_BLOCK_SIZE / SECTOR_SIZE), data, EXT4_BLOCK_SIZE / SECTOR_SIZE) != 0) {
        free_pages((void *)phys, 0);
        return -EIO;
    }

    frame_init(phys);
    *slot = phys;
    return 0;
}

/* TODO: read ahead, a request for the blocks of an extent that follow and not one for each page */
int ext4_page_read(struct inode *inode, uint64_t index, uintptr_t *out) {
    if (index >= ALIGN_UP(inode->size, PAGE_SIZE) >> PAGE_SHIFT) return -EINVAL;

    uintptr_t *slot = page_slot(ext4_i(inode), index, true);
    if (!slot) return -ENOMEM;

    if (!PG(*slot)) {
        int err = page_load(inode, index, slot);
        if (err) return err;
    }

    *out = PG(*slot);
    return 0;
}

int ext4_page_w(struct inode *inode, uint64_t index, uintptr_t *out) {
    uintptr_t *slot = page_slot(ext4_i(inode), index, true);
    if (!slot) return -ENOMEM;

    if (!PG(*slot)) {
        int err = page_load(inode, index, slot);
        if (err) return err;
    }

    *out = PG(*slot);
    return 0;
}

int ext4_page_zero(struct inode *inode, uint64_t index) {
    uintptr_t *slot = page_slot(ext4_i(inode), index, true);
    if (!slot) return -ENOMEM;

    if (PG(*slot)) return 0;

    uintptr_t phys = (uintptr_t)alloc_pages(0);
    if (!phys) return -ENOMEM;

    memset((void *)P2V(phys), 0, PAGE_SIZE);
    frame_init(phys);
    *slot = phys;
    return 0;
}

void ext4_page_dirty(struct inode *inode, uint64_t index) {
    struct ext4_info *info = ext4_i(inode);
    uintptr_t *slot = page_slot(info, index, false);

    if (!slot || !PG(*slot) || (*slot & EXT4_PG_DIRTY)) return;

    *slot |= EXT4_PG_DIRTY;
    info->ndirty++;
    __atomic_add_fetch(&info->sb->ndirty_pages, 1, 0); /* __ATOMIC_RELAXED */
    ext4_mark_dirty(inode);
}

/* a page leaves the cache: its dirty mark goes with it */
static void drop_slot(struct ext4_info *info, uintptr_t *slot) {
    if (*slot & EXT4_PG_DIRTY) {
        info->ndirty--;
        __atomic_sub_fetch(&info->sb->ndirty_pages, 1, 0); /* __ATOMIC_RELAXED */
    }

    frame_put(PG(*slot)); /* a mapping of it keeps it until it goes */
    *slot = 0;
}

static int drop_fn(struct inode *inode, uint64_t index, uintptr_t *slot, void *arg) {
    (void)index; (void)arg;

    drop_slot(ext4_i(inode), slot);
    return 0;
}

void ext4_pages_drop(struct inode *inode, uint64_t from, uint64_t to) {
    walk(inode, from, to, drop_fn, NULL);
}

static int flush_fn(struct inode *inode, uint64_t index, uintptr_t *slot, void *arg) {
    struct ext4_info *info = ext4_i(inode);
    struct ext4_sb *sb = info->sb;
    uint64_t pblk;

    (void)arg;

    if (!(*slot & EXT4_PG_DIRTY)) return 0;

    int r = ext4_map(inode, index, &pblk);

    if (r != 0) return r < 0 ? r : -EIO; /* a dirty page always has a block */

    r = blk_write(sb->dev, pblk * (EXT4_BLOCK_SIZE / SECTOR_SIZE), (void *)P2V(PG(*slot)), EXT4_BLOCK_SIZE / SECTOR_SIZE);

    if (r) {
        printk("ext4: %s: write of block %lu failed: read only now\n", sb->dev->name, pblk);
        sb->readonly = true;
        return r;
    }

    *slot &= ~EXT4_PG_DIRTY;
    info->ndirty--;
    __atomic_sub_fetch(&sb->ndirty_pages, 1, 0); /* __ATOMIC_RELAXED */
    return 0;
}

int ext4_flush_pages(struct inode *inode) {
    if (!ext4_i(inode)->ndirty) return 0;

    return walk(inode, 0, ~0ULL, flush_fn, NULL);
}

static void free_node(uintptr_t node, unsigned level) {
    uintptr_t *slots = node_slots(node);

    for (unsigned i = 0; i < TREE_SLOTS; i++) {
        if (!slots[i]) continue;

        if (level == 1) frame_put(PG(slots[i]));
        else            free_node(slots[i], level - 1);
    }

    free_pages((void *)node, 0);
}

void ext4_pages_free(struct ext4_info *info) {
    if (info->height) free_node(info->root, info->height);

    info->root   = 0;
    info->height = 0;
}
