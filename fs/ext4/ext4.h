#pragma once

#include <serika/block.h>
#include <serika/list.h>
#include <serika/lock.h>
#include <serika/mutex.h>
#include <serika/vfs.h>

/* read only ext4 with a 4 KiB block: a block is a page, so the page cache of a file is its blocks.
 * TODO: writes. creating and removing names, writing and truncating files mean allocating blocks and inodes
 * (bitmaps and group descriptors), growing the extent tree, and keeping the checksums when metadata_csum is on;
 * and with them the journal, which a crash in the middle of those leaves needing a replay */

#define EXT4_BLOCK_SIZE 4096

#define EXT4_ROOT_INO   2

#define EXT4_MAGIC      0xef53

/* feature_incompat. the ones that are not here (recovery, meta_bg, inline data, encryption, ...) refuse the mount */
#define EXT4_INCOMPAT_FILETYPE 0x0002
#define EXT4_INCOMPAT_EXTENTS  0x0040
#define EXT4_INCOMPAT_64BIT    0x0080
#define EXT4_INCOMPAT_FLEX_BG  0x0200
#define EXT4_INCOMPAT_SUPPORTED (EXT4_INCOMPAT_FILETYPE | EXT4_INCOMPAT_EXTENTS | EXT4_INCOMPAT_64BIT | EXT4_INCOMPAT_FLEX_BG)

#define EXT4_INODE_EXTENTS_FL 0x00080000

#define EXT4_EXT_MAGIC 0xf30a

struct ext4_super {
    uint32_t inodes_count;
    uint32_t blocks_count_lo;
    uint32_t r_blocks_count_lo;
    uint32_t free_blocks_count_lo;
    uint32_t free_inodes_count;
    uint32_t first_data_block;
    uint32_t log_block_size;
    uint32_t log_cluster_size;
    uint32_t blocks_per_group;
    uint32_t clusters_per_group;
    uint32_t inodes_per_group;
    uint32_t mtime;
    uint32_t wtime;
    uint16_t mnt_count;
    int16_t  max_mnt_count;
    uint16_t magic;
    uint16_t state;
    uint16_t errors;
    uint16_t minor_rev_level;
    uint32_t lastcheck;
    uint32_t checkinterval;
    uint32_t creator_os;
    uint32_t rev_level;
    uint16_t def_resuid;
    uint16_t def_resgid;
    uint32_t first_ino;
    uint16_t inode_size;
    uint16_t block_group_nr;
    uint32_t feature_compat;
    uint32_t feature_incompat;
    uint32_t feature_ro_compat;
    uint8_t  uuid[16];
    char     volume_name[16];
    uint8_t  more[0xfe - 0x88];
    uint16_t desc_size;          /* with 64bit */
    uint8_t  more2[0x150 - 0x100];
    uint32_t blocks_count_hi;
} __attribute__((packed));

_Static_assert(__builtin_offsetof(struct ext4_super, magic) == 0x38, "ext4_super magic");
_Static_assert(__builtin_offsetof(struct ext4_super, inode_size) == 0x58, "ext4_super inode_size");
_Static_assert(__builtin_offsetof(struct ext4_super, feature_incompat) == 0x60, "ext4_super feature_incompat");
_Static_assert(__builtin_offsetof(struct ext4_super, volume_name) == 0x78, "ext4_super volume_name");
_Static_assert(__builtin_offsetof(struct ext4_super, desc_size) == 0xfe, "ext4_super desc_size");
_Static_assert(__builtin_offsetof(struct ext4_super, blocks_count_hi) == 0x150, "ext4_super blocks_count_hi");

struct ext4_group_desc {
    uint32_t block_bitmap_lo;
    uint32_t inode_bitmap_lo;
    uint32_t inode_table_lo;
    uint8_t  more[0x28 - 0x0c];
    uint32_t inode_table_hi; /* only if the descriptors are 64 bytes */
} __attribute__((packed));

struct ext4_inode {
    uint16_t mode;
    uint16_t uid;
    uint32_t size_lo;
    uint32_t atime;
    uint32_t ctime;
    uint32_t mtime;
    uint32_t dtime;
    uint16_t gid;
    uint16_t links_count;
    uint32_t blocks_lo;
    uint32_t flags;
    uint32_t osd1;
    uint32_t block[15]; /* extents, or the target of a short symlink, or a device number */
    uint32_t generation;
    uint32_t file_acl_lo;
    uint32_t size_high;
    uint32_t obso_faddr;
    uint8_t  osd2[12];
    uint16_t extra_isize;
    uint16_t checksum_hi;
    uint32_t ctime_extra;
    uint32_t mtime_extra;
    uint32_t atime_extra;
} __attribute__((packed));

_Static_assert(__builtin_offsetof(struct ext4_inode, block) == 40, "ext4_inode block");
_Static_assert(__builtin_offsetof(struct ext4_inode, extra_isize) == 128, "ext4_inode extra_isize");

struct ext4_extent_header {
    uint16_t magic;
    uint16_t entries;
    uint16_t max;
    uint16_t depth;
    uint32_t generation;
} __attribute__((packed));

struct ext4_extent_idx {
    uint32_t block;   /* the first logical block it covers */
    uint32_t leaf_lo;
    uint16_t leaf_hi;
    uint16_t unused;
} __attribute__((packed));

struct ext4_extent {
    uint32_t block;
    uint16_t len;      /* over 32768: allocated, never written (reads as zeros) */
    uint16_t start_hi;
    uint32_t start_lo;
} __attribute__((packed));

struct ext4_dirent {
    uint32_t inode;    /* 0: not in use */
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[];
} __attribute__((packed));

#define EXT4_BCACHE_BUCKETS 256
#define EXT4_ICACHE_BUCKETS 256

struct ext4_bufs; /* the block cache */

struct ext4_sb {
    struct blkdev *dev;

    uint32_t inodes_count;
    uint32_t inodes_per_group;
    uint32_t inode_size;
    uint32_t desc_size;
    uint32_t first_data_block;
    uint64_t blocks_count;
    bool     wide;      /* 64bit: descriptors can be longer, inode tables above 4 G blocks */
    uint32_t id;        /* st_dev */

    /* the block cache: a block that was read stays. every block is a page of its own, and the page is mapped */
    struct mutex bcache_lock;
    list_node    bcache[EXT4_BCACHE_BUCKETS];

    spinlock_t   icache_lock;
    list_node    icache[EXT4_ICACHE_BUCKETS];
};

/* the inode's own, in inode->priv */
struct ext4_info {
    struct ext4_sb *sb;
    struct inode   *inode;
    list_node       hash;   /* icache */
    uint32_t        flags;
    uint32_t        block[15];

    uintptr_t      *pages;  /* the file's pages that were read: phys of each, 0 for not yet. buddy memory */
    size_t          npages;
};

/* super.c */
const void *ext4_bread(struct ext4_sb *sb, uint64_t blk); /* the block in memory, for good. NULL: the disk failed */
struct inode *ext4_iget(struct ext4_sb *sb, uint32_t ino); /* referenced; NULL: no such inode, or damaged */
struct inode *ext4_mount(struct blkdev *dev);              /* the root, referenced. NULL: not an ext4 we can read */
void ext4_evict(struct inode *inode);

/* inode.c */
int ext4_map(struct inode *inode, uint64_t lblk, uint64_t *pblk); /* 0 and the block, 1 for a hole, or -errno */
extern const struct inode_ops ext4_ops;

static inline struct ext4_info *ext4_i(struct inode *inode) {
    return inode->priv;
}

/* dir.c */
int  ext4_lookup(struct inode *dir, const char *name, size_t len, struct inode **out);
long ext4_getdents(struct inode *dir, void *buf, size_t n, uint64_t *pos);
