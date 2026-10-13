#include <serika/buddy.h>
#include <serika/errno.h>
#include <serika/page.h>
#include <serika/printk.h>
#include <serika/string.h>

#include "ext4.h"

#define BG_INODE_UNINIT  0x0001
#define BG_BLOCK_UNINIT  0x0002
#define BG_ITABLE_ZEROED 0x0004

static inline bool bit_test(const uint8_t *bm, uint32_t i) {
    return bm[i >> 3] & (1U << (i & 7));
}

static inline void bit_set(uint8_t *bm, uint32_t i) {
    bm[i >> 3] |= 1U << (i & 7);
}

static inline void bit_clear(uint8_t *bm, uint32_t i) {
    bm[i >> 3] &= ~(1U << (i & 7));
}

static bool gd_wide(const struct ext4_sb *sb) {
    return sb->wide && sb->desc_size >= 64;
}

/* the first block of the group, and how many it has: the last group may be short */
static uint64_t group_first(const struct ext4_sb *sb, uint32_t group) {
    return sb->first_data_block + (uint64_t)group * sb->blocks_per_group;
}

static uint32_t group_blocks(const struct ext4_sb *sb, uint32_t group) {
    uint64_t left = sb->blocks_count - group_first(sb, group);

    return left < sb->blocks_per_group ? left : sb->blocks_per_group;
}

/* the group's descriptor in the table: to read, or to change (it goes on the dirty list) */
static struct ext4_group_desc *gd_get(struct ext4_sb *sb, uint32_t group, bool write) {
    uint64_t desc = (uint64_t)group * sb->desc_size;
    uint64_t blk = sb->first_data_block + 1 + desc / EXT4_BLOCK_SIZE;
    uint8_t *table = write ? ext4_bdirty(sb, blk, EXT4_B_RAW, 0) : (uint8_t *)ext4_bread(sb, blk);

    return table ? (struct ext4_group_desc *)(table + desc % EXT4_BLOCK_SIZE) : NULL;
}

static uint64_t gd_block_bitmap(const struct ext4_sb *sb, const struct ext4_group_desc *gd) {
    return gd->block_bitmap_lo | (gd_wide(sb) ? (uint64_t)gd->block_bitmap_hi << 32 : 0);
}

static uint32_t gd_free_blocks(const struct ext4_sb *sb, const struct ext4_group_desc *gd) {
    return gd->free_blocks_count_lo | (gd_wide(sb) ? (uint32_t)gd->free_blocks_count_hi << 16 : 0);
}

static void gd_set_free_blocks(const struct ext4_sb *sb, struct ext4_group_desc *gd, uint32_t v) {
    gd->free_blocks_count_lo = v;
    if (gd_wide(sb)) gd->free_blocks_count_hi = v >> 16;
}

static uint64_t gd_inode_bitmap(const struct ext4_sb *sb, const struct ext4_group_desc *gd) {
    return gd->inode_bitmap_lo | (gd_wide(sb) ? (uint64_t)gd->inode_bitmap_hi << 32 : 0);
}

static uint64_t gd_inode_table(const struct ext4_sb *sb, const struct ext4_group_desc *gd) {
    return gd->inode_table_lo | (gd_wide(sb) ? (uint64_t)gd->inode_table_hi << 32 : 0);
}

static uint32_t gd_free_inodes(const struct ext4_sb *sb, const struct ext4_group_desc *gd) {
    return gd->free_inodes_count_lo | (gd_wide(sb) ? (uint32_t)gd->free_inodes_count_hi << 16 : 0);
}

static void gd_set_free_inodes(const struct ext4_sb *sb, struct ext4_group_desc *gd, uint32_t v) {
    gd->free_inodes_count_lo = v;
    if (gd_wide(sb)) gd->free_inodes_count_hi = v >> 16;
}

static uint32_t gd_used_dirs(const struct ext4_sb *sb, const struct ext4_group_desc *gd) {
    return gd->used_dirs_count_lo | (gd_wide(sb) ? (uint32_t)gd->used_dirs_count_hi << 16 : 0);
}

static void gd_set_used_dirs(const struct ext4_sb *sb, struct ext4_group_desc *gd, uint32_t v) {
    gd->used_dirs_count_lo = v;
    if (gd_wide(sb)) gd->used_dirs_count_hi = v >> 16;
}

static uint32_t gd_itable_unused(const struct ext4_sb *sb, const struct ext4_group_desc *gd) {
    return gd->itable_unused_lo | (gd_wide(sb) ? (uint32_t)gd->itable_unused_hi << 16 : 0);
}

static void gd_set_itable_unused(const struct ext4_sb *sb, struct ext4_group_desc *gd, uint32_t v) {
    gd->itable_unused_lo = v;
    if (gd_wide(sb)) gd->itable_unused_hi = v >> 16;
}

/* after the bitmap changed: its checksum in the descriptor, and the descriptor's own */
static void gd_seal(const struct ext4_sb *sb, uint32_t group, struct ext4_group_desc *gd, const uint8_t *bitmap) {
    if (!sb->csum) return;

    uint32_t c = ext4_crc32c(sb->csum_seed, bitmap, sb->blocks_per_group / 8);

    gd->block_bitmap_csum_lo = c;
    if (gd_wide(sb)) gd->block_bitmap_csum_hi = c >> 16;

    gd->checksum = ext4_gd_csum(sb->csum_seed, sb->desc_size, group, gd);
}

/* the same for the inode bitmap */
static void gd_seal_inodes(const struct ext4_sb *sb, uint32_t group, struct ext4_group_desc *gd, const uint8_t *bitmap) {
    if (!sb->csum) return;

    uint32_t c = ext4_crc32c(sb->csum_seed, bitmap, sb->inodes_per_group / 8);

    gd->inode_bitmap_csum_lo = c;
    if (gd_wide(sb)) gd->inode_bitmap_csum_hi = c >> 16;

    gd->checksum = ext4_gd_csum(sb->csum_seed, sb->desc_size, group, gd);
}

/* the group's bitmap as it is, if its checksum is */
static const uint8_t *load_bitmap(struct ext4_sb *sb, uint32_t group, const struct ext4_group_desc *gd) {
    const uint8_t *bm = ext4_bread(sb, gd_block_bitmap(sb, gd));
    if (!bm) return NULL;

    if (sb->csum) {
        uint32_t c = ext4_crc32c(sb->csum_seed, bm, sb->blocks_per_group / 8);
        uint32_t have = gd->block_bitmap_csum_lo | (gd_wide(sb) ? (uint32_t)gd->block_bitmap_csum_hi << 16 : 0);

        if (!gd_wide(sb)) c &= 0xffff;

        if (c != have) {
            printk("ext4: %s: group %u block bitmap checksum\n", sb->dev->name, group);
            return NULL;
        }
    }

    return bm;
}

static uint32_t find_zero(const uint8_t *bm, uint32_t from, uint32_t to) {
    for (uint32_t i = from; i < to; i++)
        if (!bit_test(bm, i)) return i;

    return to;
}

/* does the group have a copy of the super block and the descriptors: with sparse_super, 0, 1 and the powers of 3, 5 and 7 */
static bool group_has_super(const struct ext4_sb *sb, uint32_t group) {
    if (!(sb->es->feature_ro_compat & EXT4_RO_SPARSE_SUPER) || group <= 1) return true;

    for (uint32_t base = 3; base <= 7; base += 2) {
        uint64_t p = base;

        while (p < group) p *= base;

        if (p == group) return true;
    }

    return false;
}

/* a group with BLOCK_UNINIT has no bitmap on the disk yet: it is made, with what the group has of its own in it (the copy
 * of the super block and the descriptors, and the bitmaps and the inode table if they are in this group), and the bits
 * past the end of the last group. gd is to change */
static int init_block_bitmap(struct ext4_sb *sb, uint32_t group, struct ext4_group_desc *gd) {
    uint8_t *bm = ext4_bnew(sb, gd_block_bitmap(sb, gd), EXT4_B_RAW, 0);
    if (!bm) return -EIO;

    uint64_t first = group_first(sb, group);
    uint32_t nbits = group_blocks(sb, group);

    if (group_has_super(sb, group)) {
        uint32_t gdt = ((uint64_t)sb->groups_count * sb->desc_size + EXT4_BLOCK_SIZE - 1) / EXT4_BLOCK_SIZE;
        uint32_t used = 1 + gdt + sb->es->reserved_gdt_blocks;

        for (uint32_t i = 0; i < used && i < nbits; i++) bit_set(bm, i);
    }

    uint64_t itable = gd_inode_table(sb, gd);
    uint64_t itable_blocks = ((uint64_t)sb->inodes_per_group * sb->inode_size + EXT4_BLOCK_SIZE - 1) / EXT4_BLOCK_SIZE;
    uint64_t own[2] = { gd_block_bitmap(sb, gd), gd_inode_bitmap(sb, gd) };

    for (int i = 0; i < 2; i++)
        if (own[i] >= first && own[i] < first + nbits) bit_set(bm, own[i] - first);

    for (uint64_t b = itable; b < itable + itable_blocks; b++)
        if (b >= first && b < first + nbits) bit_set(bm, b - first);

    for (uint32_t i = nbits; i < EXT4_BLOCK_SIZE * 8; i++) bit_set(bm, i);

    gd->flags &= ~BG_BLOCK_UNINIT;
    gd_seal(sb, group, gd, bm);
    return 0;
}

uint64_t ext4_free_blocks_count(struct ext4_sb *sb) {
    return sb->es->free_blocks_count_lo | (sb->wide ? (uint64_t)sb->es->free_blocks_count_hi << 32 : 0);
}

static void sb_add_free(struct ext4_sb *sb, int64_t delta) {
    uint64_t v = ext4_free_blocks_count(sb) + delta;

    sb->es->free_blocks_count_lo = v;
    if (sb->wide) sb->es->free_blocks_count_hi = v >> 32;

    sb->super_dirty = true;
}

int ext4_alloc_blocks(struct ext4_sb *sb, uint64_t goal, uint32_t want, uint64_t *start, uint32_t *got) {
    int err = ext4_start_changes(sb);
    if (err) return err;

    bool in_range = goal >= sb->first_data_block && goal < sb->blocks_count;
    uint32_t first = in_range ? (goal - sb->first_data_block) / sb->blocks_per_group : 0;

    for (uint32_t n = 0; n < sb->groups_count; n++) {
        uint32_t group = (first + n) % sb->groups_count;
        const struct ext4_group_desc *gd = gd_get(sb, group, false);

        if (!gd) return -EIO;
        if (gd_free_blocks(sb, gd) == 0) continue;

        if (gd->flags & BG_BLOCK_UNINIT) {
            struct ext4_group_desc *igd = gd_get(sb, group, true);

            if (!igd || init_block_bitmap(sb, group, igd)) return -EIO;

            gd = igd;
        }

        const uint8_t *bm = load_bitmap(sb, group, gd);
        if (!bm) return -EIO;

        uint32_t nbits = group_blocks(sb, group);
        uint32_t from = (n == 0 && in_range) ? goal - group_first(sb, group) : 0;

        uint32_t bit = find_zero(bm, from, nbits);

        if (bit == nbits && from) bit = find_zero(bm, 0, from);
        if (bit >= nbits) continue;

        uint32_t len = 1;

        while (len < want && bit + len < nbits && !bit_test(bm, bit + len)) len++;

        /* it is ours: the bitmap and the descriptor are changed now */
        struct ext4_group_desc *wgd = gd_get(sb, group, true);
        uint8_t *wbm = ext4_bdirty(sb, gd_block_bitmap(sb, gd), EXT4_B_RAW, 0);

        if (!wgd || !wbm) return -EIO;

        for (uint32_t i = 0; i < len; i++) bit_set(wbm, bit + i);

        gd_set_free_blocks(sb, wgd, gd_free_blocks(sb, wgd) - len);
        gd_seal(sb, group, wgd, wbm);
        sb_add_free(sb, -(int64_t)len);

        *start = group_first(sb, group) + bit;
        *got   = len;
        return 0;
    }

    return -ENOSPC;
}

int ext4_free_blocks(struct ext4_sb *sb, uint64_t start, uint32_t count) {
    if (start < sb->first_data_block || start + count > sb->blocks_count || start + count < start) return -EINVAL;

    int err = ext4_start_changes(sb);
    if (err) return err;

    /* a block of metadata that the cache still has (a node of a tree, a block of a directory) must not be written over
     * whatever the block becomes */
    for (uint32_t i = 0; i < count; i++) ext4_bforget(sb, start + i);

    while (count) {
        uint32_t group = (start - sb->first_data_block) / sb->blocks_per_group;
        uint32_t bit = start - group_first(sb, group);
        uint32_t n = group_blocks(sb, group) - bit;

        if (n > count) n = count;

        struct ext4_group_desc *gd = gd_get(sb, group, true);
        if (!gd) return -EIO;

        if (gd->flags & BG_BLOCK_UNINIT) {                      /* nothing in it was allocated */
            printk("ext4: %s: block %lu freed in a group that is not in use\n", sb->dev->name, start);
            return -EIO;
        }

        const uint8_t *bm = load_bitmap(sb, group, gd);
        if (!bm) return -EIO;

        uint8_t *wbm = ext4_bdirty(sb, gd_block_bitmap(sb, gd), EXT4_B_RAW, 0);
        if (!wbm) return -EIO;

        uint32_t freed = 0;

        for (uint32_t i = 0; i < n; i++) {
            if (!bit_test(wbm, bit + i)) {
                printk("ext4: %s: block %lu freed twice\n", sb->dev->name, start + i);
                continue;
            }

            bit_clear(wbm, bit + i);
            freed++;
        }

        gd_set_free_blocks(sb, gd, gd_free_blocks(sb, gd) + freed);
        gd_seal(sb, group, gd, wbm);
        sb_add_free(sb, freed);

        start += n;
        count -= n;
    }

    return 0;
}

/* the inode bitmap of a group, as it is on the disk if its checksum is right */
static const uint8_t *load_inode_bitmap(struct ext4_sb *sb, uint32_t group, const struct ext4_group_desc *gd) {
    const uint8_t *bm = ext4_bread(sb, gd_inode_bitmap(sb, gd));
    if (!bm) return NULL;

    if (sb->csum) {
        uint32_t c = ext4_crc32c(sb->csum_seed, bm, sb->inodes_per_group / 8);
        uint32_t have = gd->inode_bitmap_csum_lo | (gd_wide(sb) ? (uint32_t)gd->inode_bitmap_csum_hi << 16 : 0);

        if (!gd_wide(sb)) c &= 0xffff;

        if (c != have) {
            printk("ext4: %s: group %u inode bitmap checksum\n", sb->dev->name, group);
            return NULL;
        }
    }

    return bm;
}

/* a group with INODE_UNINIT has no inode bitmap and an inode table that may not be zeros: the bitmap is made (the bits past
 * the inodes of the group are set), the table is zeroed, and the flag goes. gd is to change */
static int init_inode_group(struct ext4_sb *sb, uint32_t group, struct ext4_group_desc *gd) {
    uint8_t *bm = ext4_bnew(sb, gd_inode_bitmap(sb, gd), EXT4_B_RAW, 0);
    if (!bm) return -EIO;

    for (uint32_t i = sb->inodes_per_group; i < EXT4_BLOCK_SIZE * 8; i++) bit_set(bm, i);

    if (!(gd->flags & BG_ITABLE_ZEROED)) {
        uint64_t table = gd_inode_table(sb, gd);
        uint64_t blocks = ((uint64_t)sb->inodes_per_group * sb->inode_size + EXT4_BLOCK_SIZE - 1) / EXT4_BLOCK_SIZE;
        void *page = alloc_pages(0);

        if (!page) return -ENOMEM;

        memset((void *)P2V((uintptr_t)page), 0, EXT4_BLOCK_SIZE);

        for (uint64_t b = 0; b < blocks; b++) {
            int err = blk_write(sb->dev, (table + b) * (EXT4_BLOCK_SIZE / SECTOR_SIZE), (void *)P2V((uintptr_t)page),
                                EXT4_BLOCK_SIZE / SECTOR_SIZE);

            if (err) {
                free_pages(page, 0);
                return err;
            }
        }

        free_pages(page, 0);
    }

    gd->flags &= ~BG_INODE_UNINIT;
    gd->flags |= BG_ITABLE_ZEROED;
    gd_set_itable_unused(sb, gd, sb->inodes_per_group);
    gd_seal_inodes(sb, group, gd, bm);
    return 0;
}

static bool has_itable_unused(const struct ext4_sb *sb) {
    return sb->es->feature_ro_compat & (EXT4_RO_GDT_CSUM | EXT4_RO_METADATA_CSUM);
}

int ext4_alloc_inode(struct ext4_sb *sb, uint32_t parent, bool dir, uint32_t *ino) {
    int err = ext4_start_changes(sb);
    if (err) return err;

    /* a directory goes to the group with the most free inodes, a file stays near its directory */
    uint32_t first = parent ? (parent - 1) / sb->inodes_per_group : 0;

    if (dir) {
        uint32_t best = 0;

        for (uint32_t g = 0; g < sb->groups_count; g++) {
            const struct ext4_group_desc *gd = gd_get(sb, g, false);

            if (!gd) return -EIO;
            if (gd_free_inodes(sb, gd) > best) {
                best = gd_free_inodes(sb, gd);
                first = g;
            }
        }
    }

    for (uint32_t n = 0; n < sb->groups_count; n++) {
        uint32_t group = (first + n) % sb->groups_count;
        const struct ext4_group_desc *gd = gd_get(sb, group, false);

        if (!gd) return -EIO;
        if (gd_free_inodes(sb, gd) == 0) continue;

        struct ext4_group_desc *wgd = gd_get(sb, group, true);
        if (!wgd) return -EIO;

        if (wgd->flags & BG_INODE_UNINIT) {
            err = init_inode_group(sb, group, wgd);
            if (err) return err;
        } else if (!load_inode_bitmap(sb, group, wgd)) {
            return -EIO;
        }

        uint8_t *wbm = ext4_bdirty(sb, gd_inode_bitmap(sb, wgd), EXT4_B_RAW, 0);
        if (!wbm) return -EIO;

        uint32_t bit = find_zero(wbm, 0, sb->inodes_per_group);
        if (bit >= sb->inodes_per_group) continue;

        bit_set(wbm, bit);
        gd_set_free_inodes(sb, wgd, gd_free_inodes(sb, wgd) - 1);
        if (dir) gd_set_used_dirs(sb, wgd, gd_used_dirs(sb, wgd) + 1);

        /* the inodes after the last one in use are not read: the count says where they begin */
        if (has_itable_unused(sb) && sb->inodes_per_group - gd_itable_unused(sb, wgd) < bit + 1)
            gd_set_itable_unused(sb, wgd, sb->inodes_per_group - (bit + 1));

        gd_seal_inodes(sb, group, wgd, wbm);

        sb->es->free_inodes_count--;
        sb->super_dirty = true;

        *ino = group * sb->inodes_per_group + bit + 1;
        return 0;
    }

    return -ENOSPC;
}

int ext4_free_inode(struct ext4_sb *sb, uint32_t ino, bool dir) {
    if (ino < sb->es->first_ino || ino > sb->inodes_count) return -EINVAL;

    int err = ext4_start_changes(sb);
    if (err) return err;

    uint32_t group = (ino - 1) / sb->inodes_per_group, bit = (ino - 1) % sb->inodes_per_group;
    struct ext4_group_desc *gd = gd_get(sb, group, true);

    if (!gd) return -EIO;
    if ((gd->flags & BG_INODE_UNINIT) || !load_inode_bitmap(sb, group, gd)) return -EIO;

    uint8_t *wbm = ext4_bdirty(sb, gd_inode_bitmap(sb, gd), EXT4_B_RAW, 0);
    if (!wbm) return -EIO;

    if (!bit_test(wbm, bit)) {
        printk("ext4: %s: inode %u freed twice\n", sb->dev->name, ino);
        return -EIO;
    }

    bit_clear(wbm, bit);
    gd_set_free_inodes(sb, gd, gd_free_inodes(sb, gd) + 1);
    if (dir && gd_used_dirs(sb, gd)) gd_set_used_dirs(sb, gd, gd_used_dirs(sb, gd) - 1);
    gd_seal_inodes(sb, group, gd, wbm);

    sb->es->free_inodes_count++;
    sb->super_dirty = true;
    return 0;
}
