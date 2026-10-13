#include <serika/errno.h>
#include <serika/mutex.h>
#include <serika/page.h>
#include <serika/stat.h>
#include <serika/string.h>
#include <serika/timer.h>
#include <serika/vfs.h>

#include "ext4.h"

#define CRTIME_OFF 0x90 /* i_crtime and its extra, after the fields that struct ext4_inode has */

/* the file_type of an entry */
static uint8_t file_type(uint32_t mode) {
    switch (mode & S_IFMT) {
        case S_IFREG:  return 1;
        case S_IFDIR:  return 2;
        case S_IFCHR:  return 3;
        case S_IFBLK:  return 4;
        case S_IFIFO:  return 5;
        case S_IFSOCK: return 6;
        case S_IFLNK:  return 7;
    }

    return 0;
}

/* the inode on the disk, new: zeros but the fields that say what it is. meta_lock held */
static int inode_make(struct ext4_sb *sb, uint32_t ino, uint32_t mode, uint32_t nlink, bool extents, uint32_t gen) {
    uint64_t blk;
    uint32_t off;

    int err = ext4_inode_loc(sb, ino, &blk, &off);
    if (err) return err;

    uint8_t *raw = ext4_bdirty(sb, blk, EXT4_B_RAW, 0);
    if (!raw) return -EIO;

    struct ext4_inode *di = (struct ext4_inode *)(raw + off);
    uint64_t ns = realtime_ns(), s = ns / 1000000000ULL;
    uint32_t extra = ((s >> 32) & 3) | (uint32_t)(ns % 1000000000ULL) << 2;

    memset(di, 0, sb->inode_size);

    di->mode        = mode;
    di->links_count = nlink;
    di->generation  = gen;
    di->flags       = extents ? EXT4_INODE_EXTENTS_FL : 0;
    di->atime = di->mtime = di->ctime = s;

    if (sb->inode_size > 128) {
        di->extra_isize = sb->inode_size - 128 < 32 ? sb->inode_size - 128 : 32;
        di->atime_extra = di->mtime_extra = di->ctime_extra = extra;

        if (di->extra_isize >= 24) {
            *(uint32_t *)((uint8_t *)di + CRTIME_OFF)     = s;
            *(uint32_t *)((uint8_t *)di + CRTIME_OFF + 4) = extra;
        }
    }

    if (extents) {
        struct ext4_extent_header *h = (struct ext4_extent_header *)di->block;

        h->magic = EXT4_EXT_MAGIC;
        h->max   = 4;
    }

    if (sb->csum) ext4_inode_csum_set(ext4_inode_seed(sb->csum_seed, ino, gen), sb->inode_size, di);

    return 0;
}

/* a new inode in the cache, with a name's reference on it (the cache's) and the caller's. meta_lock held */
static int new_inode(struct inode *dir, uint32_t mode, bool extents, struct inode **out) {
    struct ext4_sb *sb = ext4_i(dir)->sb;
    uint32_t ino;

    int err = ext4_alloc_inode(sb, dir->ino, S_ISDIR(mode), &ino);
    if (err) return err;

    err = inode_make(sb, ino, mode, S_ISDIR(mode) ? 2 : 1, extents, (uint32_t)realtime_ns());

    if (err) {
        ext4_free_inode(sb, ino, S_ISDIR(mode));
        return err;
    }

    struct inode *inode = ext4_iget(sb, ino);

    if (!inode) {
        ext4_free_inode(sb, ino, S_ISDIR(mode));
        return -EIO;
    }

    *out = inode;
    return 0;
}

/* the first block of a new dir, with "." and "..". meta_lock held */
static int dir_first_block(struct inode *inode, uint32_t parent) {
    struct ext4_info *info = ext4_i(inode);
    struct ext4_sb *sb = info->sb;
    uint64_t blk;
    uint32_t got;

    int err = ext4_alloc_blocks(sb, sb->first_data_block + (uint64_t)((inode->ino - 1) / sb->inodes_per_group) * sb->blocks_per_group,
                                1, &blk, &got);
    if (err) return err;

    err = ext4_ext_insert(inode, 0, blk, 1);

    if (err) {
        ext4_free_blocks(sb, blk, 1);
        return err;
    }

    uint8_t *block = ext4_bnew(sb, blk, EXT4_B_DIR, info->seed);
    if (!block) return -EIO;

    ext4_dir_init_block(sb, block, inode->ino, parent);

    inode->size = EXT4_BLOCK_SIZE;
    return 0;
}

/* the name could not be made: the inode goes with the references it has (the caller's and the name's) */
static void inode_undo(struct inode *inode) {
    inode->nlink = 0;
    iput(inode);
    iput(inode);
}

int ext4_create(struct inode *dir, const char *name, size_t len, uint32_t mode, struct inode **out) {
    struct ext4_sb *sb = ext4_i(dir)->sb;
    struct inode *inode = NULL;
    bool isdir = S_ISDIR(mode);

    if (sb->readonly) return -EROFS;
    if (!S_ISREG(mode) && !isdir) return -EINVAL;
    if (dir->nlink == 0) return -ENOENT;                      /* retired */
    if (isdir && dir->nlink >= EXT4_LINK_MAX) return -EMLINK;

    mutex_lock(&sb->meta_lock);

    int err = ext4_start_changes(sb);

    if (!err) err = ext4_dir_make_linear(dir);
    if (!err) err = new_inode(dir, mode, true, &inode);
    if (!err && isdir) err = dir_first_block(inode, dir->ino);
    if (!err) err = ext4_dir_add(dir, name, len, inode->ino, file_type(mode));

    if (!err) {
        ext4_mark_dirty(inode);
        if (isdir) dir->nlink++;                              /* its ".." */
    }

    mutex_unlock(&sb->meta_lock);

    if (err) {
        if (inode) inode_undo(inode);
        return err;
    }

    inode_touch(dir);
    *out = inode;
    return 0;
}

int ext4_symlink(struct inode *dir, const char *name, size_t len, const char *target, struct inode **out) {
    struct ext4_sb *sb = ext4_i(dir)->sb;
    struct inode *inode = NULL;
    size_t tlen = strlen(target);
    bool fast = tlen < sizeof(((struct ext4_info *)0)->block);  /* a short target is in the inode, where the extents would be */

    if (sb->readonly) return -EROFS;
    if (tlen >= EXT4_BLOCK_SIZE) return -ENAMETOOLONG;
    if (dir->nlink == 0) return -ENOENT;

    mutex_lock(&sb->meta_lock);

    int err = ext4_start_changes(sb);

    if (!err) err = ext4_dir_make_linear(dir);
    if (!err) err = new_inode(dir, S_IFLNK | 0777, !fast, &inode);

    mutex_unlock(&sb->meta_lock);

    if (err) return err;

    /* the inode is not in any directory yet, so nobody else has it: the target is put in it without a lock */
    if (fast) {
        memcpy(ext4_i(inode)->block, target, tlen);
        inode->size = tlen;
        ext4_mark_dirty(inode);
    } else {
        long w = ext4_write(inode, target, tlen, 0);

        if (w != (long)tlen) err = w < 0 ? (int)w : -EIO;
    }

    if (!err) {
        mutex_lock(&sb->meta_lock);
        err = ext4_dir_add(dir, name, len, inode->ino, file_type(S_IFLNK));
        mutex_unlock(&sb->meta_lock);
    }

    if (err) {
        inode_undo(inode);
        return err;
    }

    inode_touch(dir);
    *out = inode;
    return 0;
}

int ext4_link(struct inode *dir, const char *name, size_t len, struct inode *inode) {
    struct ext4_sb *sb = ext4_i(dir)->sb;
    bool isdir = S_ISDIR(inode->mode);

    if (sb->readonly) return -EROFS;
    if (inode->ops != &ext4_ops) return -EPERM;               /* a fifo or the like, made somewhere else */
    if (ext4_i(inode)->sb != sb) return -EXDEV;
    if (dir->nlink == 0) return -ENOENT;
    if (isdir ? dir->nlink >= EXT4_LINK_MAX : inode->nlink >= EXT4_LINK_MAX) return -EMLINK;

    mutex_lock(&sb->meta_lock);

    int err = ext4_start_changes(sb);

    if (!err) err = ext4_dir_make_linear(dir);
    if (!err) err = ext4_dir_add(dir, name, len, inode->ino, file_type(inode->mode));

    /* a dir that moved here has this one for its ".." */
    if (!err && isdir) err = ext4_dir_set_parent(inode, dir->ino);

    mutex_unlock(&sb->meta_lock);

    if (err) return err;

    inode->nlink++;                                           /* the VFS counts a reference for each name (drop_link); an idle inode here has the cache's only, see ext4_unlink() */
    inode_touch(dir);
    inode->ctime = dir->mtime;
    inode_dirty(inode);

    if (isdir) dir->nlink++;
    return 0;
}

int ext4_unlink(struct inode *dir, const char *name, size_t len, struct inode *inode) {
    struct ext4_sb *sb = ext4_i(dir)->sb;

    if (sb->readonly) return -EROFS;

    mutex_lock(&sb->meta_lock);

    int err = ext4_start_changes(sb);

    if (!err) err = ext4_dir_remove(dir, name, len, inode->ino);

    mutex_unlock(&sb->meta_lock);

    if (err) return err;

    if (S_ISDIR(inode->mode) && dir->nlink) dir->nlink--;     /* its ".." */
    inode_touch(dir);

    /* the VFS lets go of one reference for each name that goes (drop_link). an inode in the cache has the cache's, which is
     * not a name's: while the inode keeps another name that reference is given back, and with the last name it is the one
     * that goes, so the inode is evicted (and deleted) when nobody has it open any more */
    if (inode->nlink > 1) iget(inode);

    return 0;
}

int ext4_dir_retire(struct inode *dir) {
    bool empty;

    if (ext4_i(dir)->sb->readonly) return -EROFS;

    int err = ext4_dir_empty(dir, &empty);
    if (err) return err;
    if (!empty) return -ENOTEMPTY;

    dir->nlink = 0;
    inode_dirty(dir);
    return 0;
}

/* the last reference to an inode with no names is gone: its blocks and the inode itself are given back, and the inode on
 * the disk says when it was deleted */
void ext4_inode_delete(struct inode *inode) {
    struct ext4_info *info = ext4_i(inode);
    struct ext4_sb *sb = info->sb;
    bool dir = S_ISDIR(inode->mode);

    ext4_pages_drop(inode, 0, ~0ULL);

    mutex_lock(&sb->meta_lock);

    if (ext4_start_changes(sb) == 0 && (info->flags & EXT4_INODE_EXTENTS_FL)) ext4_ext_remove(inode, 0, 1ULL << 32);

    mutex_unlock(&sb->meta_lock);

    inode->size  = 0;
    inode->ctime = realtime_ns();

    mutex_lock(&inode->lock);
    info->inode_dirty = false;
    ext4_write_inode(inode);
    mutex_unlock(&inode->lock);

    mutex_lock(&sb->meta_lock);
    ext4_free_inode(sb, inode->ino, dir);
    mutex_unlock(&sb->meta_lock);
}
