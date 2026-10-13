#pragma once

#include <serika/block.h>
#include <serika/list.h>
#include <serika/lock.h>
#include <serika/mutex.h>
#include <serika/vfs.h>

/* ext4 with a 4 KiB block: a block is a page, so the page cache of a file is its blocks. Mounting checks the checksums
 * (metadata_csum) of what it reads, and there is a dirty block cache, a flusher and sync, but every change of a file or
 * a name is still refused (-EROFS): see docs/fs/ext4.md.
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
#define EXT4_INCOMPAT_CSUM_SEED 0x2000
#define EXT4_INCOMPAT_SUPPORTED (EXT4_INCOMPAT_FILETYPE | EXT4_INCOMPAT_EXTENTS | EXT4_INCOMPAT_64BIT | EXT4_INCOMPAT_FLEX_BG | \
                                 EXT4_INCOMPAT_CSUM_SEED)

/* feature_ro_compat. a file system with any other is read only */
#define EXT4_RO_SPARSE_SUPER   0x0001
#define EXT4_RO_LARGE_FILE     0x0002
#define EXT4_RO_HUGE_FILE      0x0008
#define EXT4_RO_GDT_CSUM       0x0010
#define EXT4_RO_DIR_NLINK      0x0020
#define EXT4_RO_EXTRA_ISIZE    0x0040
#define EXT4_RO_METADATA_CSUM  0x0400
#define EXT4_RO_WRITE_OK (EXT4_RO_SPARSE_SUPER | EXT4_RO_LARGE_FILE | EXT4_RO_HUGE_FILE | EXT4_RO_GDT_CSUM | \
                          EXT4_RO_DIR_NLINK | EXT4_RO_EXTRA_ISIZE | EXT4_RO_METADATA_CSUM)

#define EXT4_STATE_VALID 0x0001 /* super block state */

#define EXT4_CSUM_CRC32C 1

#define EXT4_INODE_EXTENTS_FL 0x00080000
#define EXT4_INODE_INDEX_FL   0x00001000                /* the dir has an htree index */
#define EXT4_LINK_MAX         65000
#define EXT4_INODE_HUGE_FILE_FL 0x00040000

#define EXT4_PG_DIRTY 1UL                       /* bit 0 of the frame in the tree: the page is not written yet */
#define PG(x) ((x) & ~EXT4_PG_DIRTY)
#define EXT4_FILE_MAX (1ULL << 44)              /* 16 TiB: the blocks of a file are numbered with 32 bits */
#define EXT4_DIRTY_PAGES_MAX 8192               /* pages not written, of all files: past this a writer writes its own */

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
    uint8_t  more[0xce - 0x88];
    uint16_t reserved_gdt_blocks;
    uint8_t  more_b[0xec - 0xd0];
    uint32_t hash_seed[4];       /* htree */
    uint8_t  def_hash_version;
    uint8_t  jnl_backup_type;
    uint16_t desc_size;          /* with 64bit */
    uint8_t  more2[0x150 - 0x100];
    uint32_t blocks_count_hi;
    uint32_t r_blocks_count_hi;
    uint32_t free_blocks_count_hi;
    uint8_t  more3[0x175 - 0x15c];
    uint8_t  checksum_type;      /* 1: crc32c */
    uint8_t  more4[0x270 - 0x176];
    uint32_t checksum_seed;      /* with the CSUM_SEED feature */
    uint8_t  more5[0x3fc - 0x274];
    uint32_t checksum;
} __attribute__((packed));

_Static_assert(__builtin_offsetof(struct ext4_super, magic) == 0x38, "ext4_super magic");
_Static_assert(__builtin_offsetof(struct ext4_super, inode_size) == 0x58, "ext4_super inode_size");
_Static_assert(__builtin_offsetof(struct ext4_super, feature_incompat) == 0x60, "ext4_super feature_incompat");
_Static_assert(__builtin_offsetof(struct ext4_super, volume_name) == 0x78, "ext4_super volume_name");
_Static_assert(__builtin_offsetof(struct ext4_super, reserved_gdt_blocks) == 0xce, "ext4_super reserved_gdt_blocks");
_Static_assert(__builtin_offsetof(struct ext4_super, hash_seed) == 0xec, "ext4_super hash_seed");
_Static_assert(__builtin_offsetof(struct ext4_super, desc_size) == 0xfe, "ext4_super desc_size");
_Static_assert(__builtin_offsetof(struct ext4_super, blocks_count_hi) == 0x150, "ext4_super blocks_count_hi");
_Static_assert(__builtin_offsetof(struct ext4_super, checksum_type) == 0x175, "ext4_super checksum_type");
_Static_assert(__builtin_offsetof(struct ext4_super, checksum_seed) == 0x270, "ext4_super checksum_seed");
_Static_assert(__builtin_offsetof(struct ext4_super, checksum) == 0x3fc, "ext4_super checksum");
_Static_assert(sizeof(struct ext4_super) == 1024, "ext4_super size");

struct ext4_group_desc {
    uint32_t block_bitmap_lo;
    uint32_t inode_bitmap_lo;
    uint32_t inode_table_lo;
    uint16_t free_blocks_count_lo;
    uint16_t free_inodes_count_lo;
    uint16_t used_dirs_count_lo;
    uint16_t flags;
    uint32_t exclude_bitmap_lo;
    uint16_t block_bitmap_csum_lo;
    uint16_t inode_bitmap_csum_lo;
    uint16_t itable_unused_lo;
    uint16_t checksum;
    /* the rest only if the descriptors are 64 bytes */
    uint32_t block_bitmap_hi;
    uint32_t inode_bitmap_hi;
    uint32_t inode_table_hi;
    uint16_t free_blocks_count_hi;
    uint16_t free_inodes_count_hi;
    uint16_t used_dirs_count_hi;
    uint16_t itable_unused_hi;
    uint32_t exclude_bitmap_hi;
    uint16_t block_bitmap_csum_hi;
    uint16_t inode_bitmap_csum_hi;
    uint32_t reserved;
} __attribute__((packed));

_Static_assert(__builtin_offsetof(struct ext4_group_desc, checksum) == 0x1e, "ext4_group_desc checksum");
_Static_assert(__builtin_offsetof(struct ext4_group_desc, inode_table_hi) == 0x28, "ext4_group_desc inode_table_hi");
_Static_assert(sizeof(struct ext4_group_desc) == 64, "ext4_group_desc size");

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

enum ext4_bkind {
    EXT4_B_RAW,    /* no checksum of its own: an inode table, a group descriptors block, a bitmap, a block of file data */
    EXT4_B_EXTENT, /* a node of an extent tree: a tail with the checksum */
    EXT4_B_DIR,    /* a block of directory entries: a tail with the checksum (an htree node has none) */
};

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

    struct ext4_super *es;    /* the super block as mounted, which is written back: kmalloc */
    uint32_t csum_seed;       /* crc32c of the uuid, or the seed the super block has */
    bool     csum;            /* metadata_csum */
    bool     readonly;        /* a feature we cannot write, or the disk failed */
    bool     clean_at_mount;  /* VALID: only then is it set again by a sync */
    uint32_t blocks_per_group;
    uint32_t groups_count;
    uint32_t hash_seed[4];    /* for the htree, later */
    uint8_t  hash_version;

    struct mutex meta_lock;   /* a metadata block is changed with it held; bitmaps, group descriptors, the counters */
    list_node    dirty_list;  /* the blocks of the cache that are changed and not written (bcache_lock) */
    unsigned     ndirty;
    bool         super_dirty;  /* the free counters changed: the super block goes with the next sync (meta_lock) */
    spinlock_t   dirty_lock;
    list_node    dirty_inodes; /* the ext4_info that have something to write (dirty_lock) */
    unsigned long ndirty_pages; /* pages of files not written yet: atomic */

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
    uint32_t        generation;
    uint32_t        seed;   /* the checksum seed of this inode (blocks of extents and entries use it), 0 without metadata_csum */
    list_node       dirty;        /* on sb->dirty_inodes; linked to itself when it is not */
    bool            inode_dirty;  /* the inode has to be written */
    unsigned        ndirty;       /* pages of the file that are dirty */
    uint64_t        nblocks;      /* i_blocks: 512 byte units, the data and the extent tree's own blocks */
    uint32_t        block[15];

    uintptr_t       root;   /* the file's pages that are in the cache: a radix tree (pages.c). phys of the top node */
    unsigned        height; /* its levels; 0: no pages */
};

/* buf.c */
const void *ext4_bread(struct ext4_sb *sb, uint64_t blk); /* the block in memory, for good. NULL: the disk failed */

/* the same for a block that has a checksum: checked when it comes from the disk. NULL: the disk failed, or the
 * checksum is not right (said on the log). seed: the inode's, for an extent or a directory block */
const void *ext4_bread_kind(struct ext4_sb *sb, uint64_t blk, enum ext4_bkind kind, uint32_t seed);

/* these three with meta_lock held */
void *ext4_bdirty(struct ext4_sb *sb, uint64_t blk, enum ext4_bkind kind, uint32_t seed); /* the block to change, on the dirty list. NULL: the disk failed, or its checksum */
void *ext4_bnew(struct ext4_sb *sb, uint64_t blk, enum ext4_bkind kind, uint32_t seed);   /* a block that is allocated now: zeros, not read; dirty */
void  ext4_bfree(struct ext4_sb *sb);        /* every block of the cache: the pages and the entries. nothing is using the fs */
void  ext4_bforget(struct ext4_sb *sb, uint64_t blk);        /* the block was freed: the cache must not write it any more */
int   ext4_bflush(struct ext4_sb *sb);        /* the dirty blocks to the disk, checksums made; the disk's own cache is not flushed */

/* super.c */
struct inode *ext4_iget(struct ext4_sb *sb, uint32_t ino); /* referenced; NULL: no such inode, or damaged */
struct inode *ext4_mount(struct blkdev *dev);              /* the root, referenced. NULL: not an ext4 we can read */
void ext4_evict(struct inode *inode);
int  ext4_inode_loc(struct ext4_sb *sb, uint32_t ino, uint64_t *blk, uint32_t *off); /* where the inode is on the disk */
int  ext4_umount(struct inode *root); /* root: the mounted root, with the mount's reference and the cache's. frees the fs. -EBUSY: in use, nothing is changed */
int  ext4_write_super(struct ext4_sb *sb);     /* the super block as in memory, to the disk. meta_lock held */
int  ext4_start_changes(struct ext4_sb *sb);   /* before a change: the first since a sync puts "not clean" on the disk. meta_lock held. -EROFS */

/* inode.c */
extern const struct inode_ops ext4_ops;

static inline struct ext4_info *ext4_i(struct inode *inode) {
    return inode->priv;
}

void ext4_mark_dirty(struct inode *inode);  /* queue for the flusher; the inode lock is held */
int  ext4_write_inode(struct inode *inode); /* inode->lock held; takes meta_lock */

/* dir.c */
int  ext4_lookup(struct inode *dir, const char *name, size_t len, struct inode **out);
long ext4_getdents(struct inode *dir, void *buf, size_t n, uint64_t *pos);

/* csum.c */
uint32_t ext4_crc32c(uint32_t crc, const void *data, size_t len); /* the raw crc32c ext4 takes: no inversion, the seed is crc */
uint32_t ext4_inode_seed(uint32_t fs_seed, uint32_t ino, uint32_t gen);
void     ext4_super_csum_set(struct ext4_super *es);
bool     ext4_super_csum_ok(const struct ext4_super *es);
uint16_t ext4_gd_csum(uint32_t fs_seed, unsigned desc_size, uint32_t group, const struct ext4_group_desc *gd);
uint32_t ext4_inode_csum(uint32_t iseed, unsigned inode_size, const struct ext4_inode *di);
bool     ext4_inode_csum_ok(uint32_t iseed, unsigned inode_size, const struct ext4_inode *di);
void     ext4_inode_csum_set(uint32_t iseed, unsigned inode_size, struct ext4_inode *di);
bool     ext4_extent_csum_ok(uint32_t iseed, const void *block);
void     ext4_extent_csum_set(uint32_t iseed, void *block);
bool     ext4_dir_has_tail(const void *block);
bool     ext4_dir_csum_ok(uint32_t iseed, const void *block); /* true for a block with no tail (an htree node) */
void     ext4_dir_csum_set(uint32_t iseed, void *block);      /* nothing for a block with no tail */

/* flush.c */
void ext4_register(struct ext4_sb *sb);   /* a mounted fs is synced by the flusher and ext4_sync_all(); the first starts the flusher */
void ext4_unregister(struct ext4_sb *sb); /* waits for a sync in progress */
int  ext4_sync_fs(struct ext4_sb *sb);    /* the dirty to the disk, then clean again if nothing is. 0 or -errno */
int  ext4_sync_all(void);
int  ext4_sync_inodes(struct ext4_sb *sb); /* the queue of dirty inodes: written, each with the inode lock */

/* alloc.c: with meta_lock held; they call ext4_start_changes() */
int      ext4_alloc_blocks(struct ext4_sb *sb, uint64_t goal, uint32_t want, uint64_t *start, uint32_t *got); /* up to want in a row, near goal. 0, -ENOSPC, -EIO */
int      ext4_free_blocks(struct ext4_sb *sb, uint64_t start, uint32_t count);
uint64_t ext4_free_blocks_count(struct ext4_sb *sb); /* the super block's counter */

/* extent.c */
struct ext4_ext_found { uint64_t lblk; uint32_t len; uint64_t pblk; bool unwritten; };

int  ext4_ext_next(struct inode *inode, uint64_t lblk, struct ext4_ext_found *f); /* the extent with lblk, or the first after it. 0 (f), 1 none, -errno */
int  ext4_map(struct inode *inode, uint64_t lblk, uint64_t *pblk);                 /* 0 and the block, 1 for a hole (or unwritten), or -errno */
int  ext4_ext_insert(struct inode *inode, uint64_t lblk, uint64_t pblk, uint32_t len); /* meta_lock held. where there is none. adds len*8 to nblocks */
int  ext4_ext_remove(struct inode *inode, uint64_t from, uint64_t to);             /* meta_lock held. the mappings in [from,to) and their blocks */

/* pages.c: the pages of a file in the cache. the inode lock is held for all of these */
uintptr_t ext4_page_get(struct inode *inode, uint64_t index);                  /* the frame; 0: not in the cache */
int       ext4_page_read(struct inode *inode, uint64_t index, uintptr_t *out);  /* within the size: from the cache, or the disk */
int       ext4_page_w(struct inode *inode, uint64_t index, uintptr_t *out);     /* to change: the same, and index may be past the size */
int       ext4_page_zero(struct inode *inode, uint64_t index);                  /* a page of zeros, if there is none: for a block that was just allocated */
void      ext4_page_dirty(struct inode *inode, uint64_t index);                 /* the page was changed; the inode is queued */
void      ext4_pages_drop(struct inode *inode, uint64_t from, uint64_t to);     /* pages [from, to) leave the cache */
int       ext4_flush_pages(struct inode *inode);                                /* the dirty pages to their blocks */
void      ext4_pages_free(struct ext4_info *info);                              /* the last reference: all of them */
int      ext4_alloc_inode(struct ext4_sb *sb, uint32_t parent, bool dir, uint32_t *ino); /* near the parent's group; a directory where most are free */
int      ext4_free_inode(struct ext4_sb *sb, uint32_t ino, bool dir);

/* dir.c: changing a directory. the dir's lock and meta_lock are held */
int  ext4_dir_make_linear(struct inode *dir);  /* a dir with an index is made a plain one (before a name is added) */
int  ext4_dir_add(struct inode *dir, const char *name, size_t len, uint32_t ino, uint8_t type);
int  ext4_dir_remove(struct inode *dir, const char *name, size_t len, uint32_t ino); /* -ENOENT: not there, or another inode's */
int  ext4_dir_empty(struct inode *dir, bool *empty);
void ext4_dir_init_block(const struct ext4_sb *sb, uint8_t *block, uint32_t self, uint32_t parent); /* "." and ".." */
int  ext4_dir_set_parent(struct inode *dir, uint32_t parent);

/* namei.c: the ops that make and remove names, and the end of an inode that has none */
int  ext4_create(struct inode *dir, const char *name, size_t len, uint32_t mode, struct inode **out);
int  ext4_symlink(struct inode *dir, const char *name, size_t len, const char *target, struct inode **out);
int  ext4_link(struct inode *dir, const char *name, size_t len, struct inode *inode);
int  ext4_unlink(struct inode *dir, const char *name, size_t len, struct inode *inode);
int  ext4_dir_retire(struct inode *dir);
void ext4_inode_delete(struct inode *inode);
long ext4_write(struct inode *inode, const void *buf, size_t n, uint64_t pos);
