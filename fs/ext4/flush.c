#include <serika/errno.h>
#include <serika/mutex.h>
#include <serika/preempt.h>
#include <serika/printk.h>
#include <serika/sched.h>
#include <serika/task.h>
#include <serika/timer.h>
#include <serika/vfs.h>

#include <asm/arch.h>

#include "ext4.h"

#define EXT4_SBS_MAX  8
#define FLUSH_SECONDS 5

/* the mounted ones. a sync of one runs with this held, so an unmount that takes it waits for the sync */
static struct ext4_sb *sbs[EXT4_SBS_MAX];
static struct mutex    sbs_lock;
static bool            flusher_started;

/* everything of sb that is dirty to the disk, in the order that keeps it whole: the files' pages and inodes, the metadata
 * blocks, a flush of the disk's cache, the super block if the counters changed. then, with nothing in the middle of a
 * change (meta_lock is ours) and nothing left over, VALID again */
static int sync_fs_locked(struct ext4_sb *sb) {
    if (sb->readonly) return 0;

    int err = ext4_sync_inodes(sb);

    mutex_lock(&sb->meta_lock);

    if (!err) err = ext4_bflush(sb);
    if (!err) err = blk_flush(sb->dev);

    if (!err && sb->super_dirty) {
        err = ext4_write_super(sb);

        if (!err) sb->super_dirty = false;
        if (!err) err = blk_flush(sb->dev);
    }

    /* a change that needs no blocks only queues its inode, which the next sync takes: the list is looked at without its lock */
    if (!err && sb->clean_at_mount && !(sb->es->state & EXT4_STATE_VALID) && sb->ndirty == 0 &&
        __atomic_load_n(&sb->ndirty_pages, 0) == 0 && list_empty(&sb->dirty_inodes)) { /* __ATOMIC_RELAXED */
        sb->es->state |= EXT4_STATE_VALID;
        err = ext4_write_super(sb);

        if (!err) err = blk_flush(sb->dev);
        if (err) sb->es->state &= ~EXT4_STATE_VALID;
    }

    mutex_unlock(&sb->meta_lock);
    return err;
}

/* the queue of inodes: for each, with the inode lock, the dirty pages of the file and then the inode */
int ext4_sync_inodes(struct ext4_sb *sb) {
    int err = 0;

    for (;;) {
        uint64_t flags = spin_lock_irqsave(&sb->dirty_lock);

        if (list_empty(&sb->dirty_inodes)) {
            spin_unlock_irqrestore(&sb->dirty_lock, flags);
            break;
        }

        struct ext4_info *info = container_of(sb->dirty_inodes.next, struct ext4_info, dirty);

        list_del(&info->dirty);
        list_init(&info->dirty);

        /* the inode is held for the time of the write: a name that goes (unlink) takes the cache's reference with it, and the
         * last one to let go evicts. one that is being evicted writes itself, and is left alone */
        struct inode *inode = info->inode;
        uint32_t refs = __atomic_load_n(&inode->refs, 0); /* __ATOMIC_RELAXED */

        while (refs && !__atomic_compare_exchange_n(&inode->refs, &refs, refs + 1, false, 4, 0)) ;

        spin_unlock_irqrestore(&sb->dirty_lock, flags);

        if (!refs) continue;

        mutex_lock(&inode->lock);

        int p = ext4_flush_pages(inode);

        if (p && !err) err = p;

        if (info->inode_dirty) {
            info->inode_dirty = false;

            int e = ext4_write_inode(inode);

            if (e && !err) err = e;
        }

        mutex_unlock(&inode->lock);
        iput(inode);
    }

    return err;
}

int ext4_sync_fs(struct ext4_sb *sb) {
    mutex_lock(&sbs_lock);
    int err = sync_fs_locked(sb);
    mutex_unlock(&sbs_lock);
    return err;
}

int ext4_sync_all(void) {
    int err = 0;

    mutex_lock(&sbs_lock);

    for (int i = 0; i < EXT4_SBS_MAX; i++) {
        if (!sbs[i]) continue;

        int e = sync_fs_locked(sbs[i]);

        if (e && !err) err = e;
    }

    mutex_unlock(&sbs_lock);
    return err;
}

/* every few seconds what is dirty goes to the disk */
static void flusher(void) {
    for (;;) {
        sleep_until(rdtsc() + (uint64_t)FLUSH_SECONDS * this_cpu_read(tsc_freq_hz));
        ext4_sync_all();
    }
}

void ext4_init(void) {
    mutex_init(&sbs_lock);
}

void ext4_register(struct ext4_sb *sb) {
    mutex_lock(&sbs_lock);

    for (int i = 0; i < EXT4_SBS_MAX; i++) {
        if (sbs[i]) continue;

        sbs[i] = sb;
        break;
    }

    if (!flusher_started) {
        struct task *t = task_create(flusher, 10);

        if (t) {
            sched_add_task(t);
            flusher_started = true;
        }
    }

    mutex_unlock(&sbs_lock);
}

void ext4_unregister(struct ext4_sb *sb) {
    mutex_lock(&sbs_lock);

    for (int i = 0; i < EXT4_SBS_MAX; i++)
        if (sbs[i] == sb) sbs[i] = NULL;

    mutex_unlock(&sbs_lock);
}
