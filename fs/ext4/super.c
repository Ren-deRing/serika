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

/* one block of the cache. the page stays: nobody frees a block, and what is given out is a plain pointer.
 * TODO: let go of blocks when memory is short */
struct bcache_entry {
    list_node  node;
    uint64_t   blk;
    void      *data;
};

static int disk_read(struct ext4_sb *sb, uint64_t blk, void *data) {
    return blk_read(sb->dev, blk * (EXT4_BLOCK_SIZE / SECTOR_SIZE), data, EXT4_BLOCK_SIZE / SECTOR_SIZE);
}

const void *ext4_bread(struct ext4_sb *sb, uint64_t blk) {
    if (blk >= sb->blocks_count) return NULL;

    list_node *head = &sb->bcache[blk % EXT4_BCACHE_BUCKETS];
    const void *data = NULL;

    mutex_lock(&sb->bcache_lock);

    for (list_node *n = head->next; n != head; n = n->next) {
        struct bcache_entry *e = container_of(n, struct bcache_entry, node);

        if (e->blk == blk) {
            data = e->data;
            break;
        }
    }

    if (!data) {
        struct bcache_entry *e = kmalloc(sizeof(*e));
        void *page = alloc_pages(0);

        if (e && page) {
            e->blk  = blk;
            e->data = (void *)P2V((uintptr_t)page);

            if (disk_read(sb, blk, e->data) == 0) {
                list_add_tail(&e->node, head);
                data = e->data;
            } else {
                free_pages(page, 0);
                kfree(e);
            }
        } else {
            kfree(e);
            if (page) free_pages(page, 0);
        }
    }

    mutex_unlock(&sb->bcache_lock);
    return data;
}

/* the inode table of a group, as a block number */
static int inode_table(struct ext4_sb *sb, uint32_t group, uint64_t *blk) {
    uint64_t desc = (uint64_t)group * sb->desc_size;
    const uint8_t *table = ext4_bread(sb, sb->first_data_block + 1 + desc / EXT4_BLOCK_SIZE);
    if (!table) return -EIO;

    const struct ext4_group_desc *gd = (const void *)(table + desc % EXT4_BLOCK_SIZE);

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

/* ino is at most inodes_count, and ino 0 is none */
static struct inode *ext4_read_inode(struct ext4_sb *sb, uint32_t ino) {
    uint32_t group = (ino - 1) / sb->inodes_per_group, index = (ino - 1) % sb->inodes_per_group;
    uint64_t table;

    if (inode_table(sb, group, &table) != 0) return NULL;

    uint64_t off = (uint64_t)index * sb->inode_size;
    const uint8_t *block = ext4_bread(sb, table + off / EXT4_BLOCK_SIZE);
    if (!block) return NULL;

    const struct ext4_inode *di = (const void *)(block + off % EXT4_BLOCK_SIZE);

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
    memcpy(info->block, di->block, sizeof(info->block));
    list_init(&info->hash);

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

    uint64_t flags = spin_lock_irqsave(&sb->icache_lock);
    if (info->hash.next != &info->hash) list_del(&info->hash); /* self linked: never put in */
    spin_unlock_irqrestore(&sb->icache_lock, flags);

    for (size_t i = 0; i < info->npages; i++)
        if (info->pages[i]) frame_put(info->pages[i]); /* a mapping of it keeps it until it goes */

    if (info->pages) free_pages((void *)V2P((uintptr_t)info->pages), SIZE_TO_ORDER(info->npages * sizeof(uintptr_t)));

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

    if (blk_read(dev, 0, (void *)P2V((uintptr_t)page), EXT4_BLOCK_SIZE / SECTOR_SIZE) != 0) {
        why = "unable to read";
        goto fail;
    }

    if (es->magic != EXT4_MAGIC)                           why = "not ext4";
    else if (es->feature_incompat & 0x4)                   why = "needs journal recovery";
    else if (es->feature_incompat & ~EXT4_INCOMPAT_SUPPORTED) why = "unsupported features";
    else if (es->log_block_size != 2)                      why = "block size is not 4096";
    else if (es->rev_level < 1 || es->inode_size < 128)    why = "old format";
    else if (!es->inodes_per_group || !es->blocks_per_group) why = "damaged";

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

    mutex_init(&sb->bcache_lock);
    spin_lock_init(&sb->icache_lock);

    for (int i = 0; i < EXT4_BCACHE_BUCKETS; i++) list_init(&sb->bcache[i]);
    for (int i = 0; i < EXT4_ICACHE_BUCKETS; i++) list_init(&sb->icache[i]);

    root = ext4_iget(sb, EXT4_ROOT_INO);
    if (!root || !S_ISDIR(root->mode)) {
        why = "no root directory";
        if (root) iput(root);
        root = NULL;
        goto fail;
    }

    printk("ext4: %s \"%.16s\" %lu blocks\n", dev->name, es->volume_name, sb->blocks_count);
    goto out;

fail:
    printk("ext4: %s: %s\n", dev->name, why ? why : "out of memory");
    if (sb_page) free_pages(sb_page, sb_order);
out:
    free_pages(page, 0);
    return root;
}
