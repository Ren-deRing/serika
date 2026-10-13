#include <serika/buddy.h>
#include <serika/errno.h>
#include <serika/mm.h>
#include <serika/page.h>
#include <serika/printk.h>
#include <serika/slab.h>
#include <serika/stat.h>
#include <serika/string.h>
#include <serika/timer.h>

#include "ext4.h"

/* the inode table of a group, as a block number */
static int inode_table(struct ext4_sb *sb, uint32_t group, uint64_t *blk) {
    uint64_t desc = (uint64_t)group * sb->desc_size;
    const uint8_t *table = ext4_bread(sb, sb->first_data_block + 1 + desc / EXT4_BLOCK_SIZE);
    if (!table) return -EIO;

    const struct ext4_group_desc *gd = (const void *)(table + desc % EXT4_BLOCK_SIZE);

    if (sb->csum && ext4_gd_csum(sb->csum_seed, sb->desc_size, group, gd) != gd->checksum) {
        printk("ext4: %s: group %u descriptor checksum\n", sb->dev->name, group);
        return -EIO;
    }

    *blk = gd->inode_table_lo;
    if (sb->wide && sb->desc_size >= 64) *blk |= (uint64_t)gd->inode_table_hi << 32;
    return 0;
}

/* nanoseconds from a seconds field and its extra: the low 2 bits are the seconds' bits 32-33, the rest the ns */
static uint64_t ext4_time(const struct ext4_inode *di, uint32_t sec, uint32_t extra, size_t extra_off) {
    int64_t s = (int32_t)sec;
    uint64_t ns = 0;

    if (128 + di->extra_isize >= extra_off + 4) {
        s += (int64_t)(extra & 3) << 32;
        ns = extra >> 2;
    }

    return s < 0 ? 0 : (uint64_t)s * 1000000000ULL + ns;
}

static uint32_t decode_dev(const uint32_t *block) {
    uint32_t dev = block[0] ? block[0] : block[1];

    if (block[0]) return ((dev >> 8) & 0xff) << 8 | (dev & 0xff);

    uint32_t major = (dev & 0xfff00) >> 8, minor = (dev & 0xff) | ((dev >> 12) & 0xfff00);
    return major << 8 | (minor & 0xff);
}

/* the block of the inode table the inode is in, and where in it */
int ext4_inode_loc(struct ext4_sb *sb, uint32_t ino, uint64_t *blk, uint32_t *off) {
    uint32_t group = (ino - 1) / sb->inodes_per_group, index = (ino - 1) % sb->inodes_per_group;
    uint64_t table;

    int err = inode_table(sb, group, &table);
    if (err) return err;

    uint64_t at = (uint64_t)index * sb->inode_size;

    *blk = table + at / EXT4_BLOCK_SIZE;
    *off = at % EXT4_BLOCK_SIZE;
    return 0;
}

/* ino is at most inodes_count, and ino 0 is none */
static struct inode *ext4_read_inode(struct ext4_sb *sb, uint32_t ino) {
    uint32_t group = (ino - 1) / sb->inodes_per_group, index = (ino - 1) % sb->inodes_per_group;
    uint64_t table;

    if (inode_table(sb, group, &table) != 0) return NULL;

    uint64_t off = (uint64_t)index * sb->inode_size;
    const uint8_t *block = ext4_bread(sb, table + off / EXT4_BLOCK_SIZE);
    if (!block) return NULL;

    const struct ext4_inode *di = (const void *)(block + off % EXT4_BLOCK_SIZE);

    uint32_t seed = sb->csum ? ext4_inode_seed(sb->csum_seed, ino, di->generation) : 0;

    if (sb->csum && !ext4_inode_csum_ok(seed, sb->inode_size, di)) {
        printk("ext4: %s: inode %u checksum\n", sb->dev->name, ino);
        return NULL;
    }

    struct ext4_info *info = kmalloc(sizeof(*info));
    struct inode *inode = info ? inode_alloc(&ext4_ops, di->mode) : NULL;

    if (!inode) {
        kfree(info);
        return NULL;
    }

    memset(info, 0, sizeof(*info));
    info->sb    = sb;
    info->inode = inode;
    info->flags = di->flags;
    info->generation = di->generation;
    info->seed  = seed;
    memcpy(info->block, di->block, sizeof(info->block));
    list_init(&info->hash);
    list_init(&info->dirty);
    info->nblocks = di->blocks_lo | (uint64_t)*(const uint16_t *)di->osd2 << 32;
    if (info->flags & EXT4_INODE_HUGE_FILE_FL) info->nblocks *= EXT4_BLOCK_SIZE / SECTOR_SIZE; /* in blocks there */

    inode->priv  = info;
    inode->ino   = ino;
    inode->dev   = sb->id;
    inode->nlink = di->links_count;
    inode->size  = di->size_lo | (uint64_t)di->size_high << 32;
    inode->atime = ext4_time(di, di->atime, di->atime_extra, 140);
    inode->mtime = ext4_time(di, di->mtime, di->mtime_extra, 136);
    inode->ctime = ext4_time(di, di->ctime, di->ctime_extra, 132);

    uint32_t type = inode->mode & S_IFMT;
    if (type == S_IFCHR || type == S_IFBLK) inode->rdev = decode_dev(info->block);

    return inode;
}

/* only an inode that still has a reference is found: one whose last is gone is being evicted, and a new one is made
 * beside it. the reference is taken here, under the lock */
static struct inode *icache_find(struct ext4_sb *sb, uint32_t ino) {
    list_node *head = &sb->icache[ino % EXT4_ICACHE_BUCKETS];
    struct inode *found = NULL;

    uint64_t flags = spin_lock_irqsave(&sb->icache_lock);

    for (list_node *n = head->next; n != head && !found; n = n->next) {
        struct inode *inode = container_of(n, struct ext4_info, hash)->inode;
        if (inode->ino != ino) continue;

        uint32_t refs = __atomic_load_n(&inode->refs, 0); /* __ATOMIC_RELAXED */

        while (refs && !__atomic_compare_exchange_n(&inode->refs, &refs, refs + 1, false, 4, 0)) ;

        if (refs) found = inode;
    }

    spin_unlock_irqrestore(&sb->icache_lock, flags);
    return found;
}

struct inode *ext4_iget(struct ext4_sb *sb, uint32_t ino) {
    if (ino == 0 || ino > sb->inodes_count) return NULL;

    struct inode *inode = icache_find(sb, ino);
    if (inode) return inode;

    inode = ext4_read_inode(sb, ino);
    if (!inode) return NULL;

    /* two tasks may have read it: the one that is in first stays */
    struct inode *other = icache_find(sb, ino);

    if (other) {
        iput(inode); /* never in the cache: ext4_evict() frees what it has */
        return other;
    }

    /* the cache keeps a reference of its own: an inode that nobody has open stays, and so do the pages of its file
     * that were read. else the next program to open it reads them all again from the disk.
     * TODO: let go of inodes when memory is short */
    iget(inode);

    uint64_t flags = spin_lock_irqsave(&sb->icache_lock);
    list_add_tail(&ext4_i(inode)->hash, &sb->icache[ino % EXT4_ICACHE_BUCKETS]);
    spin_unlock_irqrestore(&sb->icache_lock, flags);

    return inode;
}

/* the inode's last reference is gone: out of the cache, and its pages */
void ext4_evict(struct inode *inode) {
    struct ext4_info *info = ext4_i(inode);
    struct ext4_sb *sb = info->sb;

    /* the flusher does not take it from here on (it is left alone once its references are gone) */
    uint64_t dflags = spin_lock_irqsave(&sb->dirty_lock);
    if (info->dirty.next != &info->dirty) {
        list_del(&info->dirty);
        list_init(&info->dirty);
    }
    spin_unlock_irqrestore(&sb->dirty_lock, dflags);

    /* what the file system has of it is on the disk first; an inode with no names is deleted */
    if (!sb->readonly) {
        if (inode->nlink == 0) {
            ext4_inode_delete(inode);
        } else if (info->ndirty || info->inode_dirty) {
            mutex_lock(&inode->lock);

            ext4_flush_pages(inode);

            if (info->inode_dirty) {
                info->inode_dirty = false;
                ext4_write_inode(inode);
            }

            mutex_unlock(&inode->lock);
        }
    }

    uint64_t flags = spin_lock_irqsave(&sb->icache_lock);
    if (info->hash.next != &info->hash) list_del(&info->hash); /* self linked: never put in */
    spin_unlock_irqrestore(&sb->icache_lock, flags);

    flags = spin_lock_irqsave(&sb->dirty_lock);
    if (info->dirty.next != &info->dirty) list_del(&info->dirty); /* a sync failed to write it: it is not coming */
    spin_unlock_irqrestore(&sb->dirty_lock, flags);

    if (info->ndirty) __atomic_sub_fetch(&sb->ndirty_pages, info->ndirty, 0); /* __ATOMIC_RELAXED */
    ext4_pages_free(info);

    kfree(info);
}

struct inode *ext4_mount(struct blkdev *dev) {
    static uint32_t next_id = 1;

    void *page = alloc_pages(0);
    if (!page) return NULL;

    const struct ext4_super *es = (const void *)(P2V((uintptr_t)page) + 1024);

    /* the hash heads are more than kmalloc gives */
    int sb_order = SIZE_TO_ORDER(sizeof(struct ext4_sb));
    void *sb_page = alloc_pages(sb_order);
    struct ext4_sb *sb = sb_page ? (struct ext4_sb *)P2V((uintptr_t)sb_page) : NULL;
    struct inode *root = NULL;
    const char *why = NULL;

    /* the superblock is 1 KiB at 1 KiB: the first sectors 2 and 3 of the 8 that are read */
    if (!sb) {
        why = "out of memory";
        goto fail;
    }

    sb->es = NULL; /* the failures that come before it is set free what there is */

    if (blk_read(dev, 0, (void *)P2V((uintptr_t)page), EXT4_BLOCK_SIZE / SECTOR_SIZE) != 0) {
        why = "unable to read";
        goto fail;
    }

    if (es->magic != EXT4_MAGIC)                           why = "not ext4";
    else if (es->feature_incompat & 0x4)                   why = "needs journal recovery";
    else if (es->feature_incompat & ~EXT4_INCOMPAT_SUPPORTED) why = "unsupported features";
    else if (es->log_block_size != 2)                      why = "block size is not 4096";
    else if (es->rev_level < 1 || es->inode_size < 128 || es->inode_size > 1024) why = "old format";
    else if (!es->inodes_per_group || !es->blocks_per_group) why = "damaged";
    else if ((es->feature_ro_compat & EXT4_RO_METADATA_CSUM) && es->checksum_type != EXT4_CSUM_CRC32C) why = "unknown checksum";
    else if ((es->feature_ro_compat & EXT4_RO_METADATA_CSUM) && !ext4_super_csum_ok(es)) why = "super block checksum";

    if (why) goto fail;

    memset(sb, 0, sizeof(*sb));
    sb->dev              = dev;
    sb->inodes_count     = es->inodes_count;
    sb->inodes_per_group = es->inodes_per_group;
    sb->inode_size       = es->inode_size;
    sb->first_data_block = es->first_data_block;
    sb->wide             = es->feature_incompat & EXT4_INCOMPAT_64BIT;
    sb->desc_size        = sb->wide && es->desc_size ? es->desc_size : 32;
    sb->blocks_count     = es->blocks_count_lo | (sb->wide ? (uint64_t)es->blocks_count_hi << 32 : 0);
    sb->id               = __atomic_fetch_add(&next_id, 1, 0); /* __ATOMIC_RELAXED */
    sb->csum             = es->feature_ro_compat & EXT4_RO_METADATA_CSUM;
    sb->csum_seed        = (es->feature_incompat & EXT4_INCOMPAT_CSUM_SEED) ? es->checksum_seed :
                           ext4_crc32c(~0U, es->uuid, sizeof(es->uuid));
    /* 2: errors were found. gdt_csum alone is a crc16 of the descriptors, which is not written here */
    sb->readonly         = (es->feature_ro_compat & ~EXT4_RO_WRITE_OK) || (es->state & 2) ||
                           ((es->feature_ro_compat & EXT4_RO_GDT_CSUM) && !(es->feature_ro_compat & EXT4_RO_METADATA_CSUM));
    sb->clean_at_mount   = es->state & EXT4_STATE_VALID;
    sb->blocks_per_group = es->blocks_per_group;
    sb->groups_count     = (sb->blocks_count - sb->first_data_block + es->blocks_per_group - 1) / es->blocks_per_group;
    memcpy(sb->hash_seed, es->hash_seed, sizeof(sb->hash_seed));
    sb->hash_version     = es->def_hash_version;

    sb->es = kmalloc(sizeof(*sb->es));
    if (!sb->es) {
        why = "out of memory";
        goto fail;
    }

    memcpy(sb->es, es, sizeof(*es));
    mutex_init(&sb->meta_lock);

    mutex_init(&sb->bcache_lock);
    spin_lock_init(&sb->icache_lock);

    list_init(&sb->dirty_list);
    spin_lock_init(&sb->dirty_lock);
    list_init(&sb->dirty_inodes);
    for (int i = 0; i < EXT4_BCACHE_BUCKETS; i++) list_init(&sb->bcache[i]);
    for (int i = 0; i < EXT4_ICACHE_BUCKETS; i++) list_init(&sb->icache[i]);

    root = ext4_iget(sb, EXT4_ROOT_INO);
    if (!root || !S_ISDIR(root->mode)) {
        why = "no root directory";
        if (root) iput(root);
        root = NULL;
        goto fail;
    }

    ext4_register(sb);
    printk("ext4: %s \"%.16s\" %lu blocks%s%s\n", dev->name, es->volume_name, sb->blocks_count,
           sb->csum ? ", checksums" : "", sb->readonly ? ", read only" : "");
    goto out;

fail:
    printk("ext4: %s: %s\n", dev->name, why ? why : "out of memory");
    if (sb_page) {
        kfree(sb->es);
        free_pages(sb_page, sb_order);
    }
out:
    free_pages(page, 0);
    return root;
}

/* the super block as it is in memory, to the disk. meta_lock held */
int ext4_write_super(struct ext4_sb *sb) {
    if (sb->csum) ext4_super_csum_set(sb->es);

    return blk_write(sb->dev, 1024 / SECTOR_SIZE, sb->es, sizeof(*sb->es) / SECTOR_SIZE);
}

/* the first change since the file system was clean: the disk says it is not before the change is made, so a crash
 * leaves a file system that fsck knows was in use. meta_lock held */
int ext4_start_changes(struct ext4_sb *sb) {
    if (sb->readonly) return -EROFS;
    if (!(sb->es->state & EXT4_STATE_VALID)) return 0;

    sb->es->state &= ~EXT4_STATE_VALID;

    int err = ext4_write_super(sb);

    if (!err) err = blk_flush(sb->dev);

    if (err) sb->es->state |= EXT4_STATE_VALID;

    return err;
}

/* the number of references an idle inode in the cache has: the cache's own. the root has the mount's too */
#define CACHE_REFS 1

int ext4_umount(struct inode *root) {
    struct ext4_sb *sb = ext4_i(root)->sb;

    /* in use: an inode that something has open, is the cwd of, or maps */
    uint64_t flags = spin_lock_irqsave(&sb->icache_lock);
    bool busy = false;

    for (int b = 0; b < EXT4_ICACHE_BUCKETS && !busy; b++) {
        list_node *head = &sb->icache[b];

        for (list_node *n = head->next; n != head; n = n->next) {
            struct inode *inode = container_of(n, struct ext4_info, hash)->inode;
            uint32_t refs = __atomic_load_n(&inode->refs, 0); /* __ATOMIC_RELAXED */

            if (refs > CACHE_REFS + (inode == root ? 1 : 0)) {
                busy = true;
                break;
            }
        }
    }

    spin_unlock_irqrestore(&sb->icache_lock, flags);

    if (busy) return -EBUSY;

    int err = ext4_sync_fs(sb);
    if (err) return err;

    ext4_unregister(sb); /* waits for a sync that is going on */

    /* the mount's reference on the root, then the cache's on every inode: each goes through ext4_evict() */
    iput(root);

    for (int b = 0; b < EXT4_ICACHE_BUCKETS; b++) {
        for (;;) {
            flags = spin_lock_irqsave(&sb->icache_lock);

            list_node *head = &sb->icache[b];
            list_node *n = head->next;
            struct inode *inode = NULL;

            if (n != head) {
                inode = container_of(n, struct ext4_info, hash)->inode;
                list_del(n);
                list_init(n); /* self linked: ext4_evict() does not take it off again */
            }

            spin_unlock_irqrestore(&sb->icache_lock, flags);

            if (!inode) break;

            iput(inode); /* the cache's reference */
        }
    }

    ext4_bfree(sb);
    kfree(sb->es);
    free_pages((void *)V2P((uintptr_t)sb), SIZE_TO_ORDER(sizeof(*sb)));
    return 0;
}
