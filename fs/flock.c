#include <serika/errno.h>
#include <serika/fcntl.h>
#include <serika/list.h>
#include <serika/lock.h>
#include <serika/preempt.h>
#include <serika/sched.h>
#include <serika/signal.h>
#include <serika/slab.h>
#include <serika/syscall.h>
#include <serika/task.h>
#include <serika/vfs.h>
#include <serika/wait.h>

#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8

/* the lock of one open file (not one fd: a dup or a fork shares it) on its inode. it goes with the file's last
 * reference, in flock_release() */
struct flock_entry {
    list_node     node;
    struct inode *inode;
    struct file  *file;
    bool          exclusive;
};

static spinlock_t        flock_lock = SPINLOCK_INITIALIZER; /* before the wait_queue lock */
static list_node         flocks = LIST_HEAD_INIT(flocks);
static struct wait_queue flock_wq = { .lock = SPINLOCK_INITIALIZER, .head = LIST_HEAD_INIT(flock_wq.head) };

/* f's lock is gone, if it had one */
static void flock_drop(struct file *f) {
    struct flock_entry *gone = NULL;

    uint64_t flags = spin_lock_irqsave(&flock_lock);

    for (list_node *n = flocks.next; n != &flocks; n = n->next) {
        struct flock_entry *e = container_of(n, struct flock_entry, node);

        if (e->file != f) continue;

        list_del(&e->node);
        gone = e;
        break;
    }

    spin_unlock_irqrestore(&flock_lock, flags);

    if (!gone) return;

    kfree(gone);
    wake_up(&flock_wq);
}

void flock_release(struct file *f) {
    if (list_empty(&flocks)) return; /* a hint: the lock of a file being closed is not racing with anyone */

    flock_drop(f);
}

/* f takes the lock as e, if no other file has one that goes against it. true: e is used */
static bool flock_try(struct file *f, bool exclusive, struct flock_entry *e) {
    bool ok = true;

    uint64_t flags = spin_lock_irqsave(&flock_lock);

    for (list_node *n = flocks.next; n != &flocks; n = n->next) {
        struct flock_entry *other = container_of(n, struct flock_entry, node);

        if (other->inode == f->inode && (exclusive || other->exclusive)) {
            ok = false;
            break;
        }
    }

    if (ok) {
        e->inode     = f->inode;
        e->file      = f;
        e->exclusive = exclusive;
        list_add_tail(&e->node, &flocks);
    }

    spin_unlock_irqrestore(&flock_lock, flags);
    return ok;
}

/* a change of lock lets go of the old one first, as linux does: it is not atomic */
long sys_flock(long fd, long op) {
    long cmd = op & ~LOCK_NB;

    if (cmd != LOCK_SH && cmd != LOCK_EX && cmd != LOCK_UN) return -EINVAL;

    struct file *f = fd_get(fd);
    if (!f) return -EBADF;

    if (f->flags & O_PATH) {
        vfs_close(f);
        return -EBADF;
    }

    flock_drop(f);

    long ret = 0;

    if (cmd != LOCK_UN) {
        struct flock_entry *e = kmalloc(sizeof(*e));

        if (!e) {
            ret = -ENOMEM;
        } else if (!flock_try(f, cmd == LOCK_EX, e)) {
            if (op & LOCK_NB) ret = -EAGAIN;
            else              ret = wait_event_interruptible(flock_wq, flock_try(f, cmd == LOCK_EX, e));

            if (ret) kfree(e);
        }
    }

    vfs_close(f);
    return ret;
}
