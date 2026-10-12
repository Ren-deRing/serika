#include <serika/errno.h>
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

    *block = ext4_bread(ext4_i(dir)->sb, pblk);
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
