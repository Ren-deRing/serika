#include <serika/errno.h>
#include <serika/printk.h>
#include <serika/string.h>

#include "ext4.h"

#define EXT_HEADER       12
#define EXT_ENTRY        12
#define EXT_ROOT_SIZE    60                                          /* i_block */
#define EXT_ROOT_MAX     4                                           /* entries in i_block */
#define EXT_NODE_MAX     ((EXT4_BLOCK_SIZE - EXT_HEADER - 4) / EXT_ENTRY) /* 340: room for the tail */
#define EXT_DEPTH_MAX    5                                           /* the tree cannot be deeper */
#define EXT_INIT_MAX_LEN 32768U

/* the way down to the leaf of a block: path[0] is the root */
struct ext_path {
    uint8_t *node;  /* the bytes of the node: i_block for the root, else the cached block */
    uint64_t blk;   /* 0 for the root */
    int      idx;   /* index node: the last entry whose key is at or before the block (0 if none); leaf: the last extent
                     * that starts at or before it, -1 if none */
};

static inline struct ext4_extent_header *hdr(uint8_t *node) {
    return (struct ext4_extent_header *)node;
}

static inline struct ext4_extent *ents(uint8_t *node) {
    return (struct ext4_extent *)(node + EXT_HEADER);
}

static inline struct ext4_extent_idx *idxs(uint8_t *node) {
    return (struct ext4_extent_idx *)(node + EXT_HEADER);
}

static inline uint32_t ext_len(const struct ext4_extent *e) {
    return e->len > EXT_INIT_MAX_LEN ? e->len - EXT_INIT_MAX_LEN : e->len;
}

static inline bool ext_unwritten(const struct ext4_extent *e) {
    return e->len > EXT_INIT_MAX_LEN;
}

static inline uint64_t ext_pstart(const struct ext4_extent *e) {
    return (uint64_t)e->start_hi << 32 | e->start_lo;
}

static inline void ext_set_pstart(struct ext4_extent *e, uint64_t p) {
    e->start_lo = p;
    e->start_hi = p >> 32;
}

static inline uint64_t idx_leaf(const struct ext4_extent_idx *ix) {
    return (uint64_t)ix->leaf_hi << 32 | ix->leaf_lo;
}

static bool root_blank(const struct ext4_info *info) {
    for (int i = 0; i < 15; i++)
        if (info->block[i]) return false;

    return true;
}

/* a file with no extent yet may have an i_block of zeros: it becomes an empty root */
static void root_init(struct ext4_info *info) {
    if (!root_blank(info)) return;

    struct ext4_extent_header *h = (struct ext4_extent_header *)info->block;

    h->magic   = EXT4_EXT_MAGIC;
    h->entries = 0;
    h->max     = EXT_ROOT_MAX;
    h->depth   = 0;
}

/* the node is going to change: its block is on the dirty list, or the inode (the root is in it) is queued */
static void touch(struct inode *inode, struct ext_path *p) {
    struct ext4_info *info = ext4_i(inode);

    if (p->blk) ext4_bdirty(info->sb, p->blk, EXT4_B_EXTENT, info->seed);
    else        ext4_mark_dirty(inode);
}

/* the way to the leaf where lblk is, or would be. the depth (the leaf's level), or -errno */
static int ext_find(struct inode *inode, uint64_t lblk, struct ext_path *path) {
    struct ext4_info *info = ext4_i(inode);
    uint8_t *node = (uint8_t *)info->block;
    uint64_t blk = 0;
    size_t size = EXT_ROOT_SIZE;
    int top = -1;

    for (int level = 0; level <= EXT_DEPTH_MAX; level++) {
        struct ext4_extent_header *h = hdr(node);

        if (h->magic != EXT4_EXT_MAGIC || EXT_HEADER + (size_t)h->entries * EXT_ENTRY > size) return -EIO;
        if (level == 0) top = h->depth;
        if (h->depth != top - level) return -EIO;

        path[level].node = node;
        path[level].blk  = blk;

        if (h->depth == 0) {
            int i = -1;

            for (unsigned k = 0; k < h->entries && ents(node)[k].block <= lblk; k++) i = k;

            path[level].idx = i;
            return level;
        }

        if (!h->entries) return -EIO;

        int i = 0;

        for (unsigned k = 0; k < h->entries && idxs(node)[k].block <= lblk; k++) i = k;

        path[level].idx = i;
        blk  = idx_leaf(&idxs(node)[i]);
        node = (uint8_t *)ext4_bread_kind(info->sb, blk, EXT4_B_EXTENT, info->seed);
        if (!node) return -EIO;

        size = EXT4_BLOCK_SIZE;
    }

    return -EIO;
}

int ext4_ext_next(struct inode *inode, uint64_t lblk, struct ext4_ext_found *f) {
    struct ext4_info *info = ext4_i(inode);
    struct ext_path path[EXT_DEPTH_MAX + 1];

    if (!(info->flags & EXT4_INODE_EXTENTS_FL)) return -EIO; /* a block map: only an old ext2 or ext3 file has one */
    if (root_blank(info)) return 1;

    int depth = ext_find(inode, lblk, path);
    if (depth < 0) return depth;

    uint8_t *node = path[depth].node;

    for (;;) {
        struct ext4_extent_header *h = hdr(node);
        struct ext4_extent *e = ents(node);

        for (unsigned i = 0; i < h->entries; i++) {
            if ((uint64_t)e[i].block + ext_len(&e[i]) <= lblk) continue;

            f->lblk      = e[i].block;
            f->len       = ext_len(&e[i]);
            f->pblk      = ext_pstart(&e[i]);
            f->unwritten = ext_unwritten(&e[i]);
            return 0;
        }

        /* the next leaf: up to a node that has another entry, then down along the first ones */
        int level = depth - 1;

        for (; level >= 0; level--)
            if (path[level].idx + 1 < hdr(path[level].node)->entries) break;

        if (level < 0) return 1;

        path[level].idx++;

        for (; level < depth; level++) {
            const struct ext4_extent_idx *ix = &idxs(path[level].node)[path[level].idx];

            node = (uint8_t *)ext4_bread_kind(info->sb, idx_leaf(ix), EXT4_B_EXTENT, info->seed);
            if (!node) return -EIO;

            path[level + 1].node = node;
            path[level + 1].idx  = 0;
        }
    }
}

int ext4_map(struct inode *inode, uint64_t lblk, uint64_t *pblk) {
    struct ext4_ext_found f;

    int err = ext4_ext_next(inode, lblk, &f);
    if (err < 0) return err;
    if (err > 0 || f.lblk > lblk || f.unwritten) return 1; /* zeros either way */

    *pblk = f.pblk + (lblk - f.lblk);
    return 0;
}

/* the first entry of the node at level changed (it was put first, or the first went): the keys above are made right, up
 * the path while the node is the first of its parent */
static void ext_fix_keys(struct inode *inode, struct ext_path *path, int level) {
    while (level > 0) {
        uint8_t *node = path[level].node;
        uint32_t key = hdr(node)->depth == 0 ? ents(node)[0].block : idxs(node)[0].block;
        struct ext_path *up = &path[level - 1];
        struct ext4_extent_idx *ix = &idxs(up->node)[up->idx];

        if (ix->block == key) return;

        touch(inode, up);
        ix->block = key;

        if (up->idx != 0) return;

        level--;
    }
}

/* a block for a node of the tree */
static int ext_node_alloc(struct inode *inode, uint64_t goal, uint64_t *blk, uint8_t **node) {
    struct ext4_info *info = ext4_i(inode);
    uint32_t got;

    int err = ext4_alloc_blocks(info->sb, goal, 1, blk, &got);
    if (err) return err;

    *node = ext4_bnew(info->sb, *blk, EXT4_B_EXTENT, info->seed);

    if (!*node) {
        ext4_free_blocks(info->sb, *blk, 1);
        return -EIO;
    }

    info->nblocks += EXT4_BLOCK_SIZE / SECTOR_SIZE;
    return 0;
}

/* the node at level is full. room is made in it or above it, and the caller finds the way again */
static int ext_split(struct inode *inode, struct ext_path *path, int level, uint64_t goal) {
    uint8_t *node = path[level].node;
    struct ext4_extent_header *h = hdr(node);
    uint64_t blk;
    uint8_t *nb;
    int err;

    if (level == 0) {
        /* the root is full: a block takes its entries, and the root has just that one */
        err = ext_node_alloc(inode, goal, &blk, &nb);
        if (err) return err;

        struct ext4_extent_header *nh = hdr(nb);

        nh->magic   = EXT4_EXT_MAGIC;
        nh->entries = h->entries;
        nh->max     = EXT_NODE_MAX;
        nh->depth   = h->depth;
        memcpy(nb + EXT_HEADER, node + EXT_HEADER, (size_t)h->entries * EXT_ENTRY);

        uint32_t key = h->depth == 0 ? ents(node)[0].block : idxs(node)[0].block;

        h->depth++;
        h->entries = 1;

        struct ext4_extent_idx *ix = idxs(node);

        ix->block   = key;
        ix->leaf_lo = blk;
        ix->leaf_hi = blk >> 32;
        ix->unused  = 0;

        ext4_mark_dirty(inode);
        return 0;
    }

    struct ext_path *up = &path[level - 1];

    if (hdr(up->node)->entries >= hdr(up->node)->max) return ext_split(inode, path, level - 1, goal);

    err = ext_node_alloc(inode, goal, &blk, &nb);
    if (err) return err;

    uint16_t keep = h->entries / 2, move = h->entries - keep;
    struct ext4_extent_header *nh = hdr(nb);

    nh->magic   = EXT4_EXT_MAGIC;
    nh->entries = move;
    nh->max     = EXT_NODE_MAX;
    nh->depth   = h->depth;
    memcpy(nb + EXT_HEADER, node + EXT_HEADER + (size_t)keep * EXT_ENTRY, (size_t)move * EXT_ENTRY);

    uint32_t key = nh->depth == 0 ? ents(nb)[0].block : idxs(nb)[0].block;

    touch(inode, &path[level]);
    h->entries = keep;

    touch(inode, up);

    struct ext4_extent_header *uh = hdr(up->node);
    struct ext4_extent_idx *ix = idxs(up->node);

    memmove(&ix[up->idx + 2], &ix[up->idx + 1], (size_t)(uh->entries - up->idx - 1) * EXT_ENTRY);

    ix[up->idx + 1].block   = key;
    ix[up->idx + 1].leaf_lo = blk;
    ix[up->idx + 1].leaf_hi = blk >> 32;
    ix[up->idx + 1].unused  = 0;
    uh->entries++;
    return 0;
}

static int ext_insert(struct inode *inode, uint64_t lblk, uint64_t pblk, uint32_t len, bool unwritten) {
    struct ext4_info *info = ext4_i(inode);
    struct ext_path path[EXT_DEPTH_MAX + 1];

    if (!(info->flags & EXT4_INODE_EXTENTS_FL)) return -EOPNOTSUPP;
    if (lblk + len > 0x100000000ULL) return -EFBIG;

    root_init(info);

    for (int tries = 0; tries < 2 * EXT_DEPTH_MAX + 2; tries++) {
        int depth = ext_find(inode, lblk, path);
        if (depth < 0) return depth;

        struct ext_path *lp = &path[depth];
        struct ext4_extent_header *h = hdr(lp->node);
        struct ext4_extent *e = ents(lp->node);
        int prev = lp->idx, next = prev + 1;

        /* in a row with the one before it: that one gets longer */
        if (!unwritten && prev >= 0 && !ext_unwritten(&e[prev]) && e[prev].block + ext_len(&e[prev]) == lblk &&
            ext_pstart(&e[prev]) + ext_len(&e[prev]) == pblk && ext_len(&e[prev]) + len <= EXT_INIT_MAX_LEN) {
            touch(inode, lp);
            e[prev].len += len;
            info->nblocks += (uint64_t)len * (EXT4_BLOCK_SIZE / SECTOR_SIZE);
            return 0;
        }

        /* or with the one after it: that one starts earlier */
        if (!unwritten && next < h->entries && !ext_unwritten(&e[next]) && lblk + len == e[next].block &&
            pblk + len == ext_pstart(&e[next]) && ext_len(&e[next]) + len <= EXT_INIT_MAX_LEN) {
            touch(inode, lp);
            e[next].block = lblk;
            ext_set_pstart(&e[next], pblk);
            e[next].len += len;
            info->nblocks += (uint64_t)len * (EXT4_BLOCK_SIZE / SECTOR_SIZE);
            if (next == 0) ext_fix_keys(inode, path, depth);
            return 0;
        }

        if (h->entries < h->max) {
            touch(inode, lp);
            memmove(&e[next + 1], &e[next], (size_t)(h->entries - next) * EXT_ENTRY);

            e[next].block = lblk;
            e[next].len   = len + (unwritten ? EXT_INIT_MAX_LEN : 0);
            ext_set_pstart(&e[next], pblk);
            h->entries++;

            info->nblocks += (uint64_t)len * (EXT4_BLOCK_SIZE / SECTOR_SIZE);
            if (next == 0) ext_fix_keys(inode, path, depth);
            return 0;
        }

        int err = ext_split(inode, path, depth, pblk);
        if (err) return err;
    }

    return -EIO;
}

int ext4_ext_insert(struct inode *inode, uint64_t lblk, uint64_t pblk, uint32_t len) {
    return ext_insert(inode, lblk, pblk, len, false);
}

/* the node at level has no entries: it is given back and its entry in the parent goes, and so up. the root stays, as an
 * empty leaf */
static void ext_drop_empty(struct inode *inode, struct ext_path *path, int level) {
    struct ext4_info *info = ext4_i(inode);

    if (level == 0) {
        hdr(path[0].node)->depth = 0;
        hdr(path[0].node)->entries = 0;
        ext4_mark_dirty(inode);
        return;
    }

    uint64_t blk = path[level].blk;

    ext4_bforget(info->sb, blk);
    ext4_free_blocks(info->sb, blk, 1);
    info->nblocks -= EXT4_BLOCK_SIZE / SECTOR_SIZE;

    struct ext_path *up = &path[level - 1];
    struct ext4_extent_header *uh = hdr(up->node);
    struct ext4_extent_idx *ix = idxs(up->node);

    touch(inode, up);
    memmove(&ix[up->idx], &ix[up->idx + 1], (size_t)(uh->entries - up->idx - 1) * EXT_ENTRY);
    uh->entries--;

    if (uh->entries == 0) ext_drop_empty(inode, path, level - 1);
    else                  ext_fix_keys(inode, path, level - 1);
}

/* [cs, ce) of the extent f goes: its blocks are given back. the middle of an extent makes two */
static int ext_cut(struct inode *inode, const struct ext4_ext_found *f, uint64_t cs, uint64_t ce) {
    struct ext4_info *info = ext4_i(inode);
    struct ext_path path[EXT_DEPTH_MAX + 1];

    int depth = ext_find(inode, f->lblk, path);
    if (depth < 0) return depth;

    struct ext_path *lp = &path[depth];
    struct ext4_extent_header *h = hdr(lp->node);

    if (lp->idx < 0 || ents(lp->node)[lp->idx].block != f->lblk) return -EIO;

    struct ext4_extent *e = &ents(lp->node)[lp->idx];
    uint64_t es = f->lblk, ee = es + f->len, ps = f->pblk;
    uint32_t flag = f->unwritten ? EXT_INIT_MAX_LEN : 0;
    int err;

    if (cs > es && ce < ee) {
        /* the front stays, the tail is a new extent; if that cannot be made, nothing changed */
        touch(inode, lp);
        e->len = (cs - es) + flag;

        err = ext_insert(inode, ce, ps + (ce - es), ee - ce, f->unwritten);

        if (err) {
            e->len = f->len + flag;
            return err;
        }

        /* the insert counted the tail again: it was counted already */
        info->nblocks -= (uint64_t)(ee - ce) * (EXT4_BLOCK_SIZE / SECTOR_SIZE);
    } else if (cs > es) {
        touch(inode, lp);
        e->len = (cs - es) + flag;
    } else if (ce < ee) {
        touch(inode, lp);
        e->block = ce;
        ext_set_pstart(e, ps + (ce - es));
        e->len = (ee - ce) + flag;
        if (lp->idx == 0) ext_fix_keys(inode, path, depth);
    } else {
        touch(inode, lp);
        memmove(e, e + 1, (size_t)(h->entries - lp->idx - 1) * EXT_ENTRY);
        h->entries--;

        if (h->entries == 0)   ext_drop_empty(inode, path, depth);
        else if (lp->idx == 0) ext_fix_keys(inode, path, depth);
    }

    err = ext4_free_blocks(info->sb, ps + (cs - es), ce - cs);
    info->nblocks -= (uint64_t)(ce - cs) * (EXT4_BLOCK_SIZE / SECTOR_SIZE);
    return err;
}

int ext4_ext_remove(struct inode *inode, uint64_t from, uint64_t to) {
    while (from < to) {
        struct ext4_ext_found f;

        int err = ext4_ext_next(inode, from, &f);
        if (err < 0) return err;
        if (err > 0 || f.lblk >= to) return 0;

        uint64_t cs = f.lblk > from ? f.lblk : from;
        uint64_t ce = f.lblk + f.len < to ? f.lblk + f.len : to;

        err = ext_cut(inode, &f, cs, ce);
        if (err) return err;

        from = ce;
    }

    return 0;
}
