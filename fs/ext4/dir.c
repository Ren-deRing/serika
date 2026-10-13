#include <serika/errno.h>
#include <serika/page.h>
#include <serika/stat.h>
#include <serika/string.h>

#include "ext4.h"

/* file_type of an entry as the S_IF* of its mode: the d_type of getdents is made from it */
static const uint32_t type_mode[] = {
    0, S_IFREG, S_IFDIR, S_IFCHR, S_IFBLK, S_IFIFO, S_IFSOCK, S_IFLNK,
};

/* the entry at off in a block, if it is whole. else -EIO: the rec_len is what the next one starts at, and a
 * damaged one would send us out of the block */
static int entry_at(const uint8_t *block, size_t off, const struct ext4_dirent **de) {
    *de = (const void *)(block + off);

    if (off + sizeof(**de) > EXT4_BLOCK_SIZE) return -EIO;

    size_t rec = (*de)->rec_len;

    if (rec < sizeof(**de) || (rec & 3) || off + rec > EXT4_BLOCK_SIZE) return -EIO;
    if (sizeof(**de) + (*de)->name_len > rec) return -EIO;

    return 0;
}

/* a dir's block b, as it is on the disk. 1: a hole */
static int dir_block(struct inode *dir, uint64_t b, const uint8_t **block) {
    uint64_t pblk;

    int err = ext4_map(dir, b, &pblk);
    if (err) return err;

    *block = ext4_bread_kind(ext4_i(dir)->sb, pblk, EXT4_B_DIR, ext4_i(dir)->seed);
    return *block ? 0 : -EIO;
}

/* a linear scan, which is also right for a dir with an htree: its index blocks look like entries that are not in use.
 * TODO: use the index */
int ext4_lookup(struct inode *dir, const char *name, size_t len, struct inode **out) {
    uint64_t nblocks = (dir->size + EXT4_BLOCK_SIZE - 1) / EXT4_BLOCK_SIZE;

    for (uint64_t b = 0; b < nblocks; b++) {
        const uint8_t *block;

        int err = dir_block(dir, b, &block);
        if (err > 0) continue;
        if (err < 0) return err;

        for (size_t off = 0; off < EXT4_BLOCK_SIZE;) {
            const struct ext4_dirent *de;

            if (entry_at(block, off, &de) != 0) return -EIO;

            if (de->inode && de->name_len == len && memcmp(de->name, name, len) == 0) {
                *out = ext4_iget(ext4_i(dir)->sb, de->inode);
                return *out ? 0 : -EIO;
            }

            off += de->rec_len;
        }
    }

    return -ENOENT;
}

/* pos is the byte of the dir the next entry is at */
long ext4_getdents(struct inode *dir, void *buf, size_t n, uint64_t *pos) {
    uint64_t nblocks = (dir->size + EXT4_BLOCK_SIZE - 1) / EXT4_BLOCK_SIZE;
    size_t done = 0;

    while (*pos / EXT4_BLOCK_SIZE < nblocks) {
        uint64_t b = *pos / EXT4_BLOCK_SIZE;
        size_t off = *pos % EXT4_BLOCK_SIZE;
        const uint8_t *block;

        int err = dir_block(dir, b, &block);

        if (err > 0) {
            *pos = (b + 1) * EXT4_BLOCK_SIZE;
            continue;
        }

        if (err < 0) return done ? (long)done : err;

        const struct ext4_dirent *de;

        if (entry_at(block, off, &de) != 0) return done ? (long)done : -EIO;

        uint64_t next = b * EXT4_BLOCK_SIZE + off + de->rec_len;

        if (de->inode) {
            uint32_t mode = de->file_type < sizeof(type_mode) / sizeof(type_mode[0]) ? type_mode[de->file_type] : 0;
            size_t r = vfs_dirent_put((char *)buf + done, n - done, de->inode, next, mode, de->name, de->name_len);

            if (!r) return done ? (long)done : -EINVAL;

            done += r;
        }

        *pos = next;
    }

    return done;
}

/* ---- changing a directory. the dir's lock and meta_lock are held. entries are in linear blocks: a new entry goes in the
 * first block with room for it, a removed one joins the one before it. the htree is not kept (see ext4_dir_make_linear) ---- */

#define DIR_TAIL_SIZE 12
#define DIR_TAIL_TYPE 0xde

static size_t rec_need(size_t name_len) {
    return ALIGN_UP(sizeof(struct ext4_dirent) + name_len, 4);
}

/* the tail of a block with checksums is an entry that is not one: it is never a place for a name */
static bool entry_is_tail(const struct ext4_sb *sb, size_t off, const struct ext4_dirent *de) {
    return sb->csum && off == EXT4_BLOCK_SIZE - DIR_TAIL_SIZE && de->inode == 0 && de->rec_len == DIR_TAIL_SIZE &&
           de->file_type == DIR_TAIL_TYPE;
}

/* a block of one entry that is not in use, as long as the block (to the tail) */
static void block_empty(const struct ext4_sb *sb, uint8_t *block) {
    struct ext4_dirent *de = (struct ext4_dirent *)block;

    memset(block, 0, EXT4_BLOCK_SIZE);
    de->rec_len = EXT4_BLOCK_SIZE - (sb->csum ? DIR_TAIL_SIZE : 0);

    if (sb->csum) {
        struct ext4_dirent *tail = (struct ext4_dirent *)(block + EXT4_BLOCK_SIZE - DIR_TAIL_SIZE);

        tail->rec_len   = DIR_TAIL_SIZE;
        tail->file_type = DIR_TAIL_TYPE;
    }
}

static void entry_put(struct ext4_dirent *de, uint32_t ino, uint8_t type, const char *name, size_t len) {
    de->inode     = ino;
    de->name_len  = len;
    de->file_type = type;
    memcpy(de->name, name, len);
}

/* a dir with an index (htree) is made a plain one: new names go in whatever block has room, which is not what the index
 * says. the index blocks are entries that are not in use (a block of a dir is read as entries whatever it is), so all that
 * is needed is the flag off, and, with checksums, the tail that a block of entries has and an index block does not. this is
 * the stop gap until the index is kept */
int ext4_dir_make_linear(struct inode *dir) {
    struct ext4_info *info = ext4_i(dir);
    struct ext4_sb *sb = info->sb;

    if (!(info->flags & EXT4_INODE_INDEX_FL)) return 0;

    for (uint64_t b = 0; b < dir->size / EXT4_BLOCK_SIZE && sb->csum; b++) {
        const uint8_t *block;
        uint64_t pblk;

        int err = dir_block(dir, b, &block);
        if (err > 0) continue;
        if (err < 0) return err;
        if (ext4_dir_has_tail(block)) continue;

        /* the last entry gives up 12 bytes at its end */
        size_t off = 0, last = 0;

        while (off < EXT4_BLOCK_SIZE) {
            const struct ext4_dirent *de;

            if (entry_at(block, off, &de) != 0) return -EIO;

            last = off;
            off += de->rec_len;
        }

        const struct ext4_dirent *le = (const void *)(block + last);
        size_t used = le->inode ? rec_need(le->name_len) : 0;

        if (le->rec_len < used + DIR_TAIL_SIZE) return -EIO;
        if (ext4_map(dir, b, &pblk) != 0) return -EIO;

        uint8_t *w = ext4_bdirty(sb, pblk, EXT4_B_DIR, info->seed);
        if (!w) return -EIO;

        ((struct ext4_dirent *)(w + last))->rec_len -= DIR_TAIL_SIZE;

        struct ext4_dirent *tail = (struct ext4_dirent *)(w + EXT4_BLOCK_SIZE - DIR_TAIL_SIZE);

        tail->inode     = 0;
        tail->rec_len   = DIR_TAIL_SIZE;
        tail->name_len  = 0;
        tail->file_type = DIR_TAIL_TYPE;
    }

    info->flags &= ~EXT4_INODE_INDEX_FL;
    ext4_mark_dirty(dir);
    return 0;
}

int ext4_dir_add(struct inode *dir, const char *name, size_t len, uint32_t ino, uint8_t type) {
    struct ext4_info *info = ext4_i(dir);
    struct ext4_sb *sb = info->sb;
    size_t need = rec_need(len);
    uint64_t nblocks = dir->size / EXT4_BLOCK_SIZE;

    for (uint64_t b = 0; b < nblocks; b++) {
        const uint8_t *block;
        uint64_t pblk;

        int err = dir_block(dir, b, &block);
        if (err > 0) continue;
        if (err < 0) return err;
        if (ext4_map(dir, b, &pblk) != 0) return -EIO;

        for (size_t off = 0; off < EXT4_BLOCK_SIZE;) {
            const struct ext4_dirent *de;

            if (entry_at(block, off, &de) != 0) return -EIO;

            if (!entry_is_tail(sb, off, de)) {
                size_t used = de->inode ? rec_need(de->name_len) : 0;

                if (de->rec_len >= used + need) {
                    uint8_t *w = ext4_bdirty(sb, pblk, EXT4_B_DIR, info->seed);
                    if (!w) return -EIO;

                    struct ext4_dirent *e = (struct ext4_dirent *)(w + off);

                    if (e->inode) {                        /* the new one takes what the old one does not use */
                        struct ext4_dirent *n = (struct ext4_dirent *)(w + off + used);

                        n->rec_len = e->rec_len - used;
                        e->rec_len = used;
                        e = n;
                    }

                    entry_put(e, ino, type, name, len);
                    return 0;
                }
            }

            off += de->rec_len;
        }
    }

    /* no room: a block more, after the last */
    uint64_t blk, goal, before;
    uint32_t got;

    if (nblocks && ext4_map(dir, nblocks - 1, &before) == 0) goal = before + 1;
    else goal = sb->first_data_block + (uint64_t)((dir->ino - 1) / sb->inodes_per_group) * sb->blocks_per_group;

    int err = ext4_alloc_blocks(sb, goal, 1, &blk, &got);
    if (err) return err;

    err = ext4_ext_insert(dir, nblocks, blk, 1);

    if (err) {
        ext4_free_blocks(sb, blk, 1);
        return err;
    }

    uint8_t *w = ext4_bnew(sb, blk, EXT4_B_DIR, info->seed);
    if (!w) return -EIO;

    block_empty(sb, w);
    entry_put((struct ext4_dirent *)w, ino, type, name, len);

    dir->size += EXT4_BLOCK_SIZE;
    ext4_mark_dirty(dir);
    return 0;
}

int ext4_dir_remove(struct inode *dir, const char *name, size_t len, uint32_t ino) {
    struct ext4_info *info = ext4_i(dir);
    uint64_t nblocks = dir->size / EXT4_BLOCK_SIZE;

    for (uint64_t b = 0; b < nblocks; b++) {
        const uint8_t *block;
        uint64_t pblk;

        int err = dir_block(dir, b, &block);
        if (err > 0) continue;
        if (err < 0) return err;

        size_t prev = (size_t)-1;

        for (size_t off = 0; off < EXT4_BLOCK_SIZE;) {
            const struct ext4_dirent *de;

            if (entry_at(block, off, &de) != 0) return -EIO;

            if (de->inode && de->name_len == len && memcmp(de->name, name, len) == 0) {
                if (de->inode != ino) return -ENOENT;          /* another file by now */
                if (ext4_map(dir, b, &pblk) != 0) return -EIO;

                uint8_t *w = ext4_bdirty(info->sb, pblk, EXT4_B_DIR, info->seed);
                if (!w) return -EIO;

                /* it joins the entry before it; the first of a block is only marked as not in use */
                if (prev != (size_t)-1) ((struct ext4_dirent *)(w + prev))->rec_len += de->rec_len;
                else                    ((struct ext4_dirent *)(w + off))->inode = 0;

                return 0;
            }

            prev = off;
            off += de->rec_len;
        }
    }

    return -ENOENT;
}

/* nothing in the dir but "." and ".." */
int ext4_dir_empty(struct inode *dir, bool *empty) {
    uint64_t nblocks = dir->size / EXT4_BLOCK_SIZE;

    *empty = true;

    for (uint64_t b = 0; b < nblocks; b++) {
        const uint8_t *block;

        int err = dir_block(dir, b, &block);
        if (err > 0) continue;
        if (err < 0) return err;

        for (size_t off = 0; off < EXT4_BLOCK_SIZE;) {
            const struct ext4_dirent *de;

            if (entry_at(block, off, &de) != 0) return -EIO;

            bool dots = de->name_len == 1 ? de->name[0] == '.' : de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.';

            if (de->inode && !dots) {
                *empty = false;
                return 0;
            }

            off += de->rec_len;
        }
    }

    return 0;
}

/* the first block of a new dir: "." and ".." */
void ext4_dir_init_block(const struct ext4_sb *sb, uint8_t *block, uint32_t self, uint32_t parent) {
    struct ext4_dirent *dot = (struct ext4_dirent *)block;

    block_empty(sb, block);

    entry_put(dot, self, 2, ".", 1);
    dot->rec_len = 12;

    struct ext4_dirent *dd = (struct ext4_dirent *)(block + 12);

    dd->rec_len = EXT4_BLOCK_SIZE - 12 - (sb->csum ? DIR_TAIL_SIZE : 0);
    entry_put(dd, parent, 2, "..", 2);
}

/* a dir that moved has another parent: its ".." */
int ext4_dir_set_parent(struct inode *dir, uint32_t parent) {
    struct ext4_info *info = ext4_i(dir);
    const uint8_t *block;
    uint64_t pblk;

    int err = dir_block(dir, 0, &block);
    if (err) return err > 0 ? -EIO : err;

    const struct ext4_dirent *dot;

    if (entry_at(block, 0, &dot) != 0) return -EIO;

    const struct ext4_dirent *dd;

    if (entry_at(block, dot->rec_len, &dd) != 0 || dd->name_len != 2 || dd->name[0] != '.' || dd->name[1] != '.') return -EIO;
    if (ext4_map(dir, 0, &pblk) != 0) return -EIO;

    uint8_t *w = ext4_bdirty(info->sb, pblk, EXT4_B_DIR, info->seed);
    if (!w) return -EIO;

    ((struct ext4_dirent *)(w + dot->rec_len))->inode = parent;
    return 0;
}
