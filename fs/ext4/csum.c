#include <serika/string.h>

#include "ext4.h"

#define CRC32C_POLY 0x82f63b78U /* reflected */

static uint32_t crc_table[256];
static bool     crc_ready;

/* two tasks may both build it: they make the same table, and the flag comes after it */
static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;

        for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ CRC32C_POLY : c >> 1;

        crc_table[i] = c;
    }

    __atomic_store_n(&crc_ready, true, 3); /* __ATOMIC_RELEASE */
}

uint32_t ext4_crc32c(uint32_t crc, const void *data, size_t len) {
    if (!__atomic_load_n(&crc_ready, 2)) crc_init(); /* __ATOMIC_ACQUIRE */

    const uint8_t *p = data;

    while (len--) crc = crc_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);

    return crc;
}

#define INODE_CSUM_LO 0x7c
#define INODE_CSUM_HI 0x82

#define SUPER_CSUM_END 0x3fc /* the checksum is the last field: the bytes before it */

#define EXTENT_HEADER 12
#define EXTENT_SIZE   12
#define DIR_TAIL      12
#define DIR_TAIL_TYPE 0xde
#define DIR_TAIL_CSUM (EXT4_BLOCK_SIZE - 4)

uint32_t ext4_inode_seed(uint32_t fs_seed, uint32_t ino, uint32_t gen) {
    uint32_t c = ext4_crc32c(fs_seed, &ino, sizeof(ino));

    return ext4_crc32c(c, &gen, sizeof(gen));
}

void ext4_super_csum_set(struct ext4_super *es) {
    es->checksum = ext4_crc32c(~0U, es, SUPER_CSUM_END);
}

bool ext4_super_csum_ok(const struct ext4_super *es) {
    return es->checksum == ext4_crc32c(~0U, es, SUPER_CSUM_END);
}

uint16_t ext4_gd_csum(uint32_t fs_seed, unsigned desc_size, uint32_t group, const struct ext4_group_desc *gd) {
    static const uint8_t zero[2];
    const unsigned at = __builtin_offsetof(struct ext4_group_desc, checksum);

    uint32_t c = ext4_crc32c(fs_seed, &group, sizeof(group));

    c = ext4_crc32c(c, gd, at);
    c = ext4_crc32c(c, zero, sizeof(zero));                    /* the checksum field, as 0 */

    if (desc_size > at + sizeof(zero)) c = ext4_crc32c(c, (const uint8_t *)gd + at + sizeof(zero), desc_size - at - sizeof(zero));

    return c & 0xffff;
}

/* the field for the high half is there if the inode has an extra part that reaches it */
static bool inode_has_hi(unsigned inode_size, const struct ext4_inode *di) {
    return inode_size > 128 && di->extra_isize >= 4;
}

/* over the inode with its checksum fields as 0 */
uint32_t ext4_inode_csum(uint32_t iseed, unsigned inode_size, const struct ext4_inode *di) {
    uint8_t copy[1024]; /* inode_size is at most this: the mount sees to it */

    memcpy(copy, di, inode_size);
    memset(copy + INODE_CSUM_LO, 0, 2);
    if (inode_has_hi(inode_size, di)) memset(copy + INODE_CSUM_HI, 0, 2);

    return ext4_crc32c(iseed, copy, inode_size);
}

bool ext4_inode_csum_ok(uint32_t iseed, unsigned inode_size, const struct ext4_inode *di) {
    uint32_t want = ext4_inode_csum(iseed, inode_size, di);
    uint32_t have = *(const uint16_t *)((const uint8_t *)di + INODE_CSUM_LO);

    if (inode_has_hi(inode_size, di)) have |= (uint32_t)di->checksum_hi << 16;
    else                              want &= 0xffff;

    return have == want;
}

void ext4_inode_csum_set(uint32_t iseed, unsigned inode_size, struct ext4_inode *di) {
    uint32_t c = ext4_inode_csum(iseed, inode_size, di);

    *(uint16_t *)((uint8_t *)di + INODE_CSUM_LO) = c;
    if (inode_has_hi(inode_size, di)) di->checksum_hi = c >> 16;
}

/* where the tail is, if the block is a node whose entries fit and leave room for it. 0: not */
static size_t extent_tail(const void *block) {
    const struct ext4_extent_header *eh = block;

    if (eh->magic != EXT4_EXT_MAGIC) return 0;

    size_t at = EXTENT_HEADER + (size_t)eh->max * EXTENT_SIZE;

    return at + 4 <= EXT4_BLOCK_SIZE ? at : 0;
}

bool ext4_extent_csum_ok(uint32_t iseed, const void *block) {
    size_t at = extent_tail(block);

    return at && *(const uint32_t *)((const uint8_t *)block + at) == ext4_crc32c(iseed, block, at);
}

void ext4_extent_csum_set(uint32_t iseed, void *block) {
    size_t at = extent_tail(block);

    if (at) *(uint32_t *)((uint8_t *)block + at) = ext4_crc32c(iseed, block, at);
}

/* the last 12 bytes of a block of entries: an entry that is not in use, 12 long, of the type 0xde */
bool ext4_dir_has_tail(const void *block) {
    const struct ext4_dirent *t = (const void *)((const uint8_t *)block + EXT4_BLOCK_SIZE - DIR_TAIL);

    return t->inode == 0 && t->rec_len == DIR_TAIL && t->name_len == 0 && t->file_type == DIR_TAIL_TYPE;
}

bool ext4_dir_csum_ok(uint32_t iseed, const void *block) {
    if (!ext4_dir_has_tail(block)) return true; /* an htree node: it has its own, which is not checked yet */

    return *(const uint32_t *)((const uint8_t *)block + DIR_TAIL_CSUM) == ext4_crc32c(iseed, block, EXT4_BLOCK_SIZE - DIR_TAIL);
}

void ext4_dir_csum_set(uint32_t iseed, void *block) {
    if (!ext4_dir_has_tail(block)) return;

    *(uint32_t *)((uint8_t *)block + DIR_TAIL_CSUM) = ext4_crc32c(iseed, block, EXT4_BLOCK_SIZE - DIR_TAIL);
}
