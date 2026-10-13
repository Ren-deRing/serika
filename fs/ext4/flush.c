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

/* everything of sb that is dirty to the disk, in the order that keeps it whole: the metadata blocks, then a flush of
 * the disk's cache. then, with nothing in the middle of a change (meta_lock is ours), VALID again.
 * TODO: the data pages of files and the inodes, before these (stage 2) */
static int sync_fs_locked(struct ext4_sb *sb) {
    if (sb->readonly) return 0;

    mutex_lock(&sb->meta_lock);

    int err = ext4_bflush(sb);

    if (!err) err = blk_flush(sb->dev);

    if (!err && sb->clean_at_mount && !(sb->es->state & EXT4_STATE_VALID)) {
        sb->es->state |= EXT4_STATE_VALID;
        err = ext4_write_super(sb);

        if (!err) err = blk_flush(sb->dev);
        if (err) sb->es->state &= ~EXT4_STATE_VALID;
    }

    mutex_unlock(&sb->meta_lock);
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
