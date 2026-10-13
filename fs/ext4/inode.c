#include <serika/buddy.h>
#include <serika/errno.h>
#include <serika/mm.h>
#include <serika/mutex.h>
#include <serika/page.h>
#include <serika/stat.h>
#include <serika/string.h>

#include "ext4.h"

#define EXT_ROOT_SIZE 60 /* i_block */

static long ext4_read(struct inode *inode, void *buf, size_t n, uint64_t pos) {
    uint8_t *to = buf;

    if (pos >= inode->size) return 0;
    if (n > inode->size - pos) n = inode->size - pos;

    for (size_t done = 0; done < n;) {
        size_t off = (pos + done) & (PAGE_SIZE - 1);
        size_t c   = PAGE_SIZE - off;
        if (c > n - done) c = n - done;

        uintptr_t phys;
        int err = ext4_page_read(inode, (pos + done) >> PAGE_SHIFT, &phys);
        if (err) return done ? (long)done : err;

        memcpy(to + done, (void *)P2V(phys + off), c);
        done += c;
    }

    return n;
}

/* the page of the file at index for a mapping, the mapping's reference taken. 0: past the end, or the disk failed */
static uintptr_t ext4_getpage(struct inode *inode, uint64_t index) {
    uintptr_t phys;

    if (ext4_page_read(inode, index, &phys) != 0) return 0;

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

/* a block for every page of [first, last] that has none, with a page of zeros in the cache for each. *end is the first page
 * that has no block when this stops (last + 1 or more when all have). meta_lock is taken here, and the inode is queued
 * before it is dropped */
static int alloc_holes(struct inode *inode, uint64_t first, uint64_t last, uint64_t *end) {
    struct ext4_info *info = ext4_i(inode);
    struct ext4_sb *sb = info->sb;
    uint64_t idx = first;
    int err = 0;

    mutex_lock(&sb->meta_lock);

    while (idx <= last) {
        struct ext4_ext_found f;
        int r = ext4_ext_next(inode, idx, &f);

        if (r < 0) {
            err = r;
            break;
        }

        if (r == 0 && f.lblk <= idx) {                          /* inside an extent */
            if (f.unwritten) {
                err = -EOPNOTSUPP;
                break;
            }

            idx = f.lblk + f.len;
            continue;
        }

        uint64_t hole_end = (r == 0 && f.lblk <= last) ? f.lblk : last + 1;

        while (idx < hole_end) {
            uint64_t want = hole_end - idx, start, goal, before;
            uint32_t got;

            if (want > 32768) want = 32768;

            /* zeros in the cache first: a block that is mapped and has no page would be read from the disk, stale */
            for (uint64_t i = 0; i < want && !err; i++) err = ext4_page_zero(inode, idx + i);
            if (err) break;

            if (idx > 0 && ext4_map(inode, idx - 1, &before) == 0) goal = before + 1;
            else goal = sb->first_data_block + (uint64_t)((inode->ino - 1) / sb->inodes_per_group) * sb->blocks_per_group;

            err = ext4_alloc_blocks(sb, goal, want, &start, &got);
            if (err) break;

            err = ext4_ext_insert(inode, idx, start, got);

            if (err) {
                ext4_free_blocks(sb, start, got);
                break;
            }

            idx += got;
        }

        if (err) break;
    }

    ext4_mark_dirty(inode);                                      /* the extents and nblocks changed */
    mutex_unlock(&sb->meta_lock);

    *end = idx;
    return err;
}

long ext4_write(struct inode *inode, const void *buf, size_t n, uint64_t pos) {
    struct ext4_info *info = ext4_i(inode);
    const uint8_t *from = buf;

    if (n == 0) return 0;
    if (info->sb->readonly) return -EROFS;
    if (!(info->flags & EXT4_INODE_EXTENTS_FL)) return -EOPNOTSUPP;
    if (pos + n > EXT4_FILE_MAX) return -EFBIG;

    uint64_t first = pos >> PAGE_SHIFT, last = (pos + n - 1) >> PAGE_SHIFT, end;
    int err = alloc_holes(inode, first, last, &end);

    /* the bytes that have a block: from the start of the write to the end of the last page that has one */
    uint64_t limit = end << PAGE_SHIFT;

    if (limit <= pos) return err ? err : -EIO;
    if (n > limit - pos) n = limit - pos;

    size_t done = 0;

    while (done < n) {
        size_t off = (pos + done) & (PAGE_SIZE - 1);
        size_t c   = PAGE_SIZE - off;
        if (c > n - done) c = n - done;

        uintptr_t phys;
        uint64_t index = (pos + done) >> PAGE_SHIFT;

        if (ext4_page_w(inode, index, &phys) != 0) break;

        memcpy((void *)P2V(phys + off), from + done, c);
        ext4_page_dirty(inode, index);
        done += c;
    }

    if (pos + done > inode->size) inode->size = pos + done;

    /* too many pages not written: this file's go now */
    if (__atomic_load_n(&info->sb->ndirty_pages, 0) > EXT4_DIRTY_PAGES_MAX) ext4_flush_pages(inode); /* __ATOMIC_RELAXED */

    return done ? (long)done : -EIO;
}

/* bytes [off, off + len) of one page are zeros, if the page has a block (else they are zeros already) */
static int zero_in_page(struct inode *inode, uint64_t index, size_t off, size_t len) {
    uint64_t pblk;
    int r = ext4_map(inode, index, &pblk);

    if (r < 0) return r;
    if (r > 0) return 0;

    uintptr_t phys;
    int err = ext4_page_w(inode, index, &phys);
    if (err) return err;

    memset((void *)P2V(phys + off), 0, len);
    ext4_page_dirty(inode, index);
    return 0;
}

static int ext4_truncate(struct inode *inode, uint64_t size) {
    struct ext4_info *info = ext4_i(inode);
    struct ext4_sb *sb = info->sb;

    if (sb->readonly) return -EROFS;
    if (!(info->flags & EXT4_INODE_EXTENTS_FL)) return -EOPNOTSUPP;
    if (size > EXT4_FILE_MAX) return -EFBIG;

    if (size >= inode->size) {                               /* longer: a hole */
        inode->size = size;
        return 0;
    }

    uint64_t first = ALIGN_UP(size, PAGE_SIZE) >> PAGE_SHIFT;

    if (size & (PAGE_SIZE - 1)) {
        int err = zero_in_page(inode, size >> PAGE_SHIFT, size & (PAGE_SIZE - 1), PAGE_SIZE - (size & (PAGE_SIZE - 1)));
        if (err) return err;
    }

    ext4_pages_drop(inode, first, ~0ULL);

    mutex_lock(&sb->meta_lock);

    int err = ext4_start_changes(sb);

    if (!err) err = ext4_ext_remove(inode, first, 1ULL << 32);

    ext4_mark_dirty(inode);
    mutex_unlock(&sb->meta_lock);

    if (!err) inode->size = size;
    return err;
}

static int ext4_punch_hole(struct inode *inode, uint64_t off, uint64_t len) {
    struct ext4_info *info = ext4_i(inode);
    struct ext4_sb *sb = info->sb;
    uint64_t end = off + len;

    if (sb->readonly) return -EROFS;
    if (!(info->flags & EXT4_INODE_EXTENTS_FL)) return -EOPNOTSUPP;

    uint64_t first = ALIGN_UP(off, PAGE_SIZE) >> PAGE_SHIFT, last = ALIGN_DOWN(end, PAGE_SIZE) >> PAGE_SHIFT;

    if (first > last)                                        /* inside one page */
        return zero_in_page(inode, off >> PAGE_SHIFT, off & (PAGE_SIZE - 1), len);

    int err = 0;

    if (off & (PAGE_SIZE - 1)) err = zero_in_page(inode, off >> PAGE_SHIFT, off & (PAGE_SIZE - 1), PAGE_SIZE - (off & (PAGE_SIZE - 1)));
    if (!err && (end & (PAGE_SIZE - 1))) err = zero_in_page(inode, last, 0, end & (PAGE_SIZE - 1));
    if (err) return err;

    if (first == last) return 0;

    ext4_pages_drop(inode, first, last);

    mutex_lock(&sb->meta_lock);

    err = ext4_start_changes(sb);

    if (!err) err = ext4_ext_remove(inode, first, last);

    ext4_mark_dirty(inode);
    mutex_unlock(&sb->meta_lock);
    return err;
}

/* on the sb's list, once. the inode lock is held; the list's lock is a spin lock, taken for nothing else */
void ext4_mark_dirty(struct inode *inode) {
    struct ext4_info *info = ext4_i(inode);
    struct ext4_sb *sb = info->sb;

    info->inode_dirty = true;

    uint64_t flags = spin_lock_irqsave(&sb->dirty_lock);

    if (info->dirty.next == &info->dirty) list_add_tail(&info->dirty, &sb->dirty_inodes);

    spin_unlock_irqrestore(&sb->dirty_lock, flags);
}

/* the extra field of a time: the nanoseconds with 2 more bits of seconds, if the inode has room for it (else it is as it is) */
static uint32_t time_extra(const struct ext4_inode *di, uint64_t ns, size_t extra_off, uint32_t old) {
    if (128 + di->extra_isize < extra_off + 4) return old;

    return ((ns / 1000000000ULL >> 32) & 3) | (uint32_t)(ns % 1000000000ULL) << 2;
}

/* the fields that the kernel has go to the inode's slot in the table; the rest (owner, attributes) stay as they are.
 * the inode lock is held; meta_lock is taken here */
int ext4_write_inode(struct inode *inode) {
    struct ext4_info *info = ext4_i(inode);
    struct ext4_sb *sb = info->sb;
    uint64_t blk;
    uint32_t off;

    int err = ext4_inode_loc(sb, inode->ino, &blk, &off);
    if (err) return err;

    mutex_lock(&sb->meta_lock);

    err = ext4_start_changes(sb);

    uint8_t *raw = err ? NULL : ext4_bdirty(sb, blk, EXT4_B_RAW, 0);

    if (!err && !raw) err = -EIO;

    if (!err) {
        struct ext4_inode *di = (struct ext4_inode *)(raw + off);
        uint64_t blocks = info->nblocks;

        if (info->flags & EXT4_INODE_HUGE_FILE_FL) blocks /= EXT4_BLOCK_SIZE / SECTOR_SIZE;

        di->mode        = inode->mode;
        di->links_count = inode->nlink;
        di->size_lo     = inode->size;
        di->size_high   = inode->size >> 32;
        di->flags       = info->flags;
        if (!inode->nlink) di->dtime = inode->ctime / 1000000000ULL; /* deleted */
        di->blocks_lo   = blocks;
        *(uint16_t *)di->osd2 = blocks >> 32;
        memcpy(di->block, info->block, sizeof(di->block));

        di->atime = inode->atime / 1000000000ULL;
        di->mtime = inode->mtime / 1000000000ULL;
        di->ctime = inode->ctime / 1000000000ULL;
        di->atime_extra = time_extra(di, inode->atime, 140, di->atime_extra);
        di->mtime_extra = time_extra(di, inode->mtime, 136, di->mtime_extra);
        di->ctime_extra = time_extra(di, inode->ctime, 132, di->ctime_extra);

        if (sb->csum) ext4_inode_csum_set(info->seed, sb->inode_size, di);
    }

    mutex_unlock(&sb->meta_lock);
    return err;
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
    .punch_hole = ext4_punch_hole,
    .getpage    = ext4_getpage,
    .getdents   = ext4_getdents,
    .evict      = ext4_evict,
    .mark_dirty = ext4_mark_dirty,
};
