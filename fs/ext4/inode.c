#include <serika/buddy.h>
#include <serika/errno.h>
#include <serika/mm.h>
#include <serika/page.h>
#include <serika/stat.h>
#include <serika/string.h>

#include "ext4.h"

#define EXT_ROOT_SIZE 60 /* i_block */
#define EXT_DEPTH_MAX 5  /* the tree cannot be deeper */

/* the index entries or extents of a node, if it is whole. size: the bytes the node has */
static int ext_node(const uint8_t *node, size_t size, const struct ext4_extent_header **h) {
    *h = (const void *)node;

    if ((*h)->magic != EXT4_EXT_MAGIC) return -EIO;
    if (sizeof(**h) + (size_t)(*h)->entries * sizeof(struct ext4_extent) > size) return -EIO;
    return 0;
}

int ext4_map(struct inode *inode, uint64_t lblk, uint64_t *pblk) {
    struct ext4_info *info = ext4_i(inode);

    if (!(info->flags & EXT4_INODE_EXTENTS_FL)) return -EIO; /* a block map: only an old ext2 or ext3 file has one */

    const uint8_t *node = (const uint8_t *)info->block;
    size_t size = EXT_ROOT_SIZE;

    for (int level = 0; level <= EXT_DEPTH_MAX; level++) {
        const struct ext4_extent_header *h;

        if (ext_node(node, size, &h) != 0) return -EIO;

        if (h->depth == 0) {
            const struct ext4_extent *e = (const void *)(h + 1), *hit = NULL;

            for (unsigned i = 0; i < h->entries && e[i].block <= lblk; i++) hit = &e[i];

            if (!hit) return 1;

            uint32_t len = hit->len;
            bool unwritten = len > 32768;

            if (unwritten) len -= 32768;

            if (lblk >= (uint64_t)hit->block + len || unwritten) return 1; /* zeros either way */

            *pblk = ((uint64_t)hit->start_hi << 32 | hit->start_lo) + (lblk - hit->block);
            return 0;
        }

        const struct ext4_extent_idx *ix = (const void *)(h + 1), *hit = NULL;

        for (unsigned i = 0; i < h->entries && ix[i].block <= lblk; i++) hit = &ix[i];

        if (!hit) return 1;

        node = ext4_bread_kind(info->sb, (uint64_t)hit->leaf_hi << 32 | hit->leaf_lo, EXT4_B_EXTENT, info->seed);
        if (!node) return -EIO;

        size = EXT4_BLOCK_SIZE;
    }

    return -EIO;
}

/* slots for npages pages. the array comes from buddy: kmalloc stops at a page */
static int ext4_grow(struct ext4_info *info, size_t npages) {
    if (npages <= info->npages) return 0;

    int order = SIZE_TO_ORDER(npages * sizeof(uintptr_t));

    uintptr_t phys = (uintptr_t)alloc_pages(order);
    if (!phys) return -ENOMEM;

    uintptr_t *pages = (uintptr_t *)P2V(phys);
    memset(pages, 0, ORDER_TO_SIZE(order));

    if (info->pages) {
        memcpy(pages, info->pages, info->npages * sizeof(uintptr_t));
        free_pages((void *)V2P((uintptr_t)info->pages), SIZE_TO_ORDER(info->npages * sizeof(uintptr_t)));
    }

    info->pages  = pages;
    info->npages = ORDER_TO_SIZE(order) / sizeof(uintptr_t);
    return 0;
}

/* the page of the file at index, from the disk the first time. the cache keeps its reference.
 * TODO: read ahead, a request for the blocks of an extent that follow and not one for each page */
static int ext4_page(struct inode *inode, uint64_t index, uintptr_t *out) {
    struct ext4_info *info = ext4_i(inode);

    if (index >= ALIGN_UP(inode->size, PAGE_SIZE) >> PAGE_SHIFT) return -EINVAL;

    int err = ext4_grow(info, index + 1);
    if (err) return err;

    if (!info->pages[index]) {
        uint64_t pblk;

        err = ext4_map(inode, index, &pblk);
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
        info->pages[index] = phys;
    }

    *out = info->pages[index];
    return 0;
}

static long ext4_read(struct inode *inode, void *buf, size_t n, uint64_t pos) {
    uint8_t *to = buf;

    if (pos >= inode->size) return 0;
    if (n > inode->size - pos) n = inode->size - pos;

    for (size_t done = 0; done < n;) {
        size_t off = (pos + done) & (PAGE_SIZE - 1);
        size_t c   = PAGE_SIZE - off;
        if (c > n - done) c = n - done;

        uintptr_t phys;
        int err = ext4_page(inode, (pos + done) >> PAGE_SHIFT, &phys);
        if (err) return done ? (long)done : err;

        memcpy(to + done, (void *)P2V(phys + off), c);
        done += c;
    }

    return n;
}

/* the page of the file at index for a mapping, the mapping's reference taken. 0: past the end, or the disk failed */
static uintptr_t ext4_getpage(struct inode *inode, uint64_t index) {
    uintptr_t phys;

    if (ext4_page(inode, index, &phys) != 0) return 0;

    frame_get(phys);
    return phys;
}

static long ext4_readlink(struct inode *inode, char *buf, size_t n) {
    size_t len = inode->size;
    if (len > n) len = n;

    /* a short target is in the inode where the extents would be */
    if (!(ext4_i(inode)->flags & EXT4_INODE_EXTENTS_FL) && inode->size < EXT_ROOT_SIZE) {
        memcpy(buf, ext4_i(inode)->block, len);
        return len;
    }

    return ext4_read(inode, buf, len, 0);
}

/* the file system is read only: every way to change it says so */
static int ext4_create(struct inode *dir, const char *name, size_t len, uint32_t mode, struct inode **out) {
    (void)dir; (void)name; (void)len; (void)mode; (void)out;
    return -EROFS;
}

static int ext4_symlink(struct inode *dir, const char *name, size_t len, const char *target, struct inode **out) {
    (void)dir; (void)name; (void)len; (void)target; (void)out;
    return -EROFS;
}

static int ext4_link(struct inode *dir, const char *name, size_t len, struct inode *inode) {
    (void)dir; (void)name; (void)len; (void)inode;
    return -EROFS;
}

static int ext4_unlink(struct inode *dir, const char *name, size_t len, struct inode *inode) {
    (void)dir; (void)name; (void)len; (void)inode;
    return -EROFS;
}

static int ext4_dir_retire(struct inode *dir) {
    (void)dir;
    return -EROFS;
}

static long ext4_write(struct inode *inode, const void *buf, size_t n, uint64_t pos) {
    (void)inode; (void)buf; (void)n; (void)pos;
    return -EROFS;
}

static int ext4_truncate(struct inode *inode, uint64_t size) {
    (void)inode; (void)size;
    return -EROFS;
}

const struct inode_ops ext4_ops = {
    .lookup     = ext4_lookup,
    .create     = ext4_create,
    .symlink    = ext4_symlink,
    .readlink   = ext4_readlink,
    .link       = ext4_link,
    .unlink     = ext4_unlink,
    .dir_retire = ext4_dir_retire,
    .read       = ext4_read,
    .write      = ext4_write,
    .truncate   = ext4_truncate,
    .getpage    = ext4_getpage,
    .getdents   = ext4_getdents,
    .evict      = ext4_evict,
};
