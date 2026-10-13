#include <serika/string.h>

#include <fs/ext4/ext4.h>
#include <tests/test.h>

/* crc32c as ext4 takes it: no inversion at the ends, so the usual value of "123456789" (0xe3069283) is complemented */
bool test_ext4_crc32c() {
    TEST_ASSERT(ext4_crc32c(~0U, "123456789", 9) == 0x1cf96d7c);
    TEST_ASSERT(ext4_crc32c(0, "", 0) == 0);

    /* 32 zero bytes: 0x8a9136aa is the usual value (rfc 3720) */
    uint8_t zeros[32];
    memset(zeros, 0, sizeof(zeros));
    TEST_ASSERT(ext4_crc32c(~0U, zeros, sizeof(zeros)) == ~0x8a9136aaU);

    /* in pieces it is the same as whole */
    uint32_t c = ext4_crc32c(~0U, "1234", 4);
    c = ext4_crc32c(c, "56789", 5);
    TEST_ASSERT(c == 0x1cf96d7c);
    return true;
}

#define INODE_SIZE 256

/* the seed of a file system made up */
#define FS_SEED 0x12345678U

bool test_ext4_csum_kinds() {
    /* the super block: the first 0x3fc bytes */
    struct ext4_super es;
    memset(&es, 0x5a, sizeof(es));
    ext4_super_csum_set(&es);
    TEST_ASSERT(ext4_super_csum_ok(&es));
    es.mnt_count ^= 1;
    TEST_ASSERT(!ext4_super_csum_ok(&es));

    /* a group descriptor of 64 bytes: its own checksum field does not count, the rest does */
    struct ext4_group_desc gd;
    memset(&gd, 0x33, sizeof(gd));
    uint16_t a = ext4_gd_csum(FS_SEED, 64, 7, &gd);
    gd.checksum = 0xffff;
    TEST_ASSERT(ext4_gd_csum(FS_SEED, 64, 7, &gd) == a);
    TEST_ASSERT(ext4_gd_csum(FS_SEED, 64, 8, &gd) != a);       /* the group is in it */
    gd.inode_table_hi ^= 1;
    TEST_ASSERT(ext4_gd_csum(FS_SEED, 64, 7, &gd) != a);       /* so is the second half */
    TEST_ASSERT(ext4_gd_csum(FS_SEED, 32, 7, &gd) != ext4_gd_csum(FS_SEED, 64, 7, &gd));

    /* an inode of 256 bytes: with its extra part the checksum is 32 bits, without it 16 */
    uint8_t raw[INODE_SIZE];
    struct ext4_inode *di = (struct ext4_inode *)raw;
    memset(raw, 0x11, sizeof(raw));
    di->extra_isize = 32;
    uint32_t iseed = ext4_inode_seed(FS_SEED, 12, 99);
    TEST_ASSERT(iseed != ext4_inode_seed(FS_SEED, 13, 99) && iseed != ext4_inode_seed(FS_SEED, 12, 98));

    ext4_inode_csum_set(iseed, INODE_SIZE, di);
    TEST_ASSERT(ext4_inode_csum_ok(iseed, INODE_SIZE, di));
    TEST_ASSERT(di->checksum_hi == (ext4_inode_csum(iseed, INODE_SIZE, di) >> 16));
    raw[200] ^= 0x80;
    TEST_ASSERT(!ext4_inode_csum_ok(iseed, INODE_SIZE, di));
    raw[200] ^= 0x80;

    di->extra_isize = 0;                                       /* no field for the high half */
    ext4_inode_csum_set(iseed, INODE_SIZE, di);
    TEST_ASSERT(ext4_inode_csum_ok(iseed, INODE_SIZE, di));

    /* an extent block: the tail is after the room for the entries */
    static uint8_t block[4096];
    struct ext4_extent_header *eh = (struct ext4_extent_header *)block;
    memset(block, 0x22, sizeof(block));
    eh->magic = EXT4_EXT_MAGIC;
    eh->max = 340;
    ext4_extent_csum_set(iseed, block);
    TEST_ASSERT(ext4_extent_csum_ok(iseed, block));
    block[100] ^= 1;
    TEST_ASSERT(!ext4_extent_csum_ok(iseed, block));
    eh->max = 400;                                             /* a tail past the block: damaged */
    TEST_ASSERT(!ext4_extent_csum_ok(iseed, block));
    return true;
}

bool test_ext4_csum_dir() {
    static uint8_t block[4096];
    uint32_t iseed = ext4_inode_seed(FS_SEED, 2, 0);

    /* one entry to the tail, and the tail: inode 0, rec_len 12, name_len 0, type 0xde */
    memset(block, 0, sizeof(block));
    struct ext4_dirent *de = (struct ext4_dirent *)block;
    de->inode = 5;
    de->rec_len = 4096 - 12;
    de->name_len = 1;
    de->name[0] = 'a';
    struct ext4_dirent *tail = (struct ext4_dirent *)(block + 4096 - 12);
    tail->rec_len = 12;
    tail->file_type = 0xde;

    TEST_ASSERT(ext4_dir_has_tail(block));
    ext4_dir_csum_set(iseed, block);
    TEST_ASSERT(ext4_dir_csum_ok(iseed, block));
    de->name[0] = 'b';
    TEST_ASSERT(!ext4_dir_csum_ok(iseed, block));

    /* an htree node has no tail: an empty entry as long as the block, and it is not checked */
    memset(block, 0, sizeof(block));
    de->rec_len = 4096;
    TEST_ASSERT(!ext4_dir_has_tail(block));
    TEST_ASSERT(ext4_dir_csum_ok(iseed, block));
    ext4_dir_csum_set(iseed, block);                           /* and nothing is written */
    TEST_ASSERT(block[4095] == 0 && block[4092] == 0);
    return true;
}
