#include <serika/errno.h>
#include <serika/fcntl.h>
#include <serika/mm.h>
#include <serika/poll.h>
#include <serika/preempt.h>
#include <serika/sched.h>
#include <serika/signal.h>
#include <serika/slab.h>
#include <serika/string.h>
#include <serika/syscall.h>
#include <serika/task.h>
#include <serika/vfs.h>
#include <serika/wait.h>

#define SFD_CLOEXEC  O_CLOEXEC
#define SFD_NONBLOCK O_NONBLOCK

/* what a read gives, linux's */
struct signalfd_siginfo {
    uint32_t ssi_signo;
    int32_t  ssi_errno;
    int32_t  ssi_code;
    uint32_t ssi_pid;
    uint32_t ssi_uid;
    int32_t  ssi_fd;
    uint32_t ssi_tid;
    uint32_t ssi_band;
    uint32_t ssi_overrun;
    uint32_t ssi_trapno;
    int32_t  ssi_status;
    int32_t  ssi_int;
    uint64_t ssi_ptr;
    uint64_t ssi_utime;
    uint64_t ssi_stime;
    uint64_t ssi_addr;
    uint8_t  pad[48];
};

/* the signals are the reader's own: a signalfd is no queue, it reads what is pending on the task that asks */
struct signalfd {
    struct wait_queue wq; /* nobody wakes it: send_signal() wakes the task, which looks again */
    uint64_t          mask; /* atomic */
};

static const struct inode_ops signalfd_ops;

static struct signalfd *signalfd_of(struct inode *inode) {
    return inode->priv;
}

static uint64_t mask_of(struct signalfd *s) {
    return __atomic_load_n(&s->mask, 0); /* __ATOMIC_RELAXED */
}

static long signalfd_read(struct file *f, void *buf, size_t n) {
    struct signalfd *s = signalfd_of(f->inode);
    size_t done = 0;

    if (n < sizeof(struct signalfd_siginfo)) return -EINVAL;

    for (;;) {
        struct ksiginfo info;
        int sig = signal_take(mask_of(s), &info);

        if (!sig) {
            if (done) break; /* what was there */

            if (f->flags & O_NONBLOCK) return -EAGAIN;

            int err = wait_event_interruptible(s->wq, signal_pending_in(mask_of(s)));
            if (err) return err;

            continue;
        }

        struct signalfd_siginfo out;

        memset(&out, 0, sizeof(out));
        out.ssi_signo = sig;
        out.ssi_code  = info.code;
        out.ssi_pid   = info.pid;
        out.ssi_addr  = info.addr;

        memcpy((char *)buf + done, &out, sizeof(out));
        done += sizeof(out);

        if (n - done < sizeof(out)) break;
    }

    return done;
}

static uint32_t signalfd_poll(struct inode *inode, uint32_t fflags, struct poll_table *pt) {
    struct signalfd *s = signalfd_of(inode);

    (void)fflags;

    poll_wait(pt, &s->wq);

    return signal_pending_in(mask_of(s)) ? POLLIN : 0;
}

static void signalfd_evict(struct inode *inode) {
    kfree(inode->priv);
}

static const struct inode_ops signalfd_ops = {
    .stream      = true,
    .stream_read = signalfd_read,
    .poll        = signalfd_poll,
    .evict       = signalfd_evict,
};

long sys_signalfd4(long fd, long umask, long size, long flags) {
    uint64_t mask;

    if (flags & ~(SFD_CLOEXEC | SFD_NONBLOCK)) return -EINVAL;
    if (size != sizeof(mask)) return -EINVAL;
    if (copy_from_user(&mask, umask, sizeof(mask)) != 0) return -EFAULT;

    mask &= ~SIG_UNBLOCKABLE;

    if (fd != -1) {
        struct file *f = fd_get(fd);
        if (!f) return -EBADF;

        if (f->inode->ops != &signalfd_ops) {
            vfs_close(f);
            return -EINVAL;
        }

        __atomic_store_n(&signalfd_of(f->inode)->mask, mask, 0); /* __ATOMIC_RELAXED */
        wake_up(&signalfd_of(f->inode)->wq);

        vfs_close(f);
        return fd;
    }

    struct signalfd *s = kmalloc(sizeof(*s));
    if (!s) return -ENOMEM;

    wait_queue_init(&s->wq);
    s->mask = mask;

    struct inode *inode = inode_alloc(&signalfd_ops, 0600);
    if (!inode) {
        kfree(s);
        return -ENOMEM;
    }

    inode->priv = s; /* evict frees it, from here on */

    return anon_fd(inode, O_RDWR | (flags & SFD_NONBLOCK), flags & SFD_CLOEXEC);
}
