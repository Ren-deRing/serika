#include <serika/buddy.h>
#include <serika/cpu.h>
#include <serika/errno.h>
#include <serika/mm.h>
#include <serika/page.h>
#include <serika/poll.h>
#include <serika/preempt.h>
#include <serika/sched.h>
#include <serika/signal.h>
#include <serika/slab.h>
#include <serika/string.h>
#include <serika/syscall.h>
#include <serika/task.h>
#include <serika/timer.h>
#include <serika/vfs.h>

#include <asm/arch.h>

#define FD_SETSIZE 1024

/* what a poll is registered on. each lives until the poll is over */
struct poll_entry {
    struct wait_queue *wq;
    struct wait_entry  we;
};

struct poll_table {
    struct poll_entry *ents;
    unsigned           n;
    unsigned           max;
};

void poll_wait(struct poll_table *pt, struct wait_queue *wq) {
    if (!pt || pt->n == pt->max) return;

    struct poll_entry *e = &pt->ents[pt->n++];

    e->wq = wq;
    e->we = (struct wait_entry){ 0 };

    prepare_to_wait(wq, &e->we); /* and the task is sleeping from here on, until something wakes it */
}

/* the readiness of every fd. pt registers on their queues as it goes */
static unsigned poll_pass(struct pollfd *fds, unsigned nfds, struct poll_table *pt) {
    unsigned count = 0;

    for (unsigned i = 0; i < nfds; i++) {
        struct pollfd *p = &fds[i];
        uint32_t mask;

        p->revents = 0;
        if (p->fd < 0) continue;

        struct file *f = fd_get(p->fd);
        if (!f) {
            p->revents = POLLNVAL;
            count++;
            continue;
        }

        mask = vfs_poll(f, pt);
        vfs_close(f);

        mask &= (uint16_t)p->events | POLLERR | POLLHUP;
        p->revents = mask;

        if (mask) count++;
    }

    return count;
}

struct poll_sleep {
    struct timer      timer;
    struct wait_queue wq;
    volatile bool     timed_out;
};

static void poll_timeout(struct timer *t) {
    struct poll_sleep *s = container_of(t, struct poll_sleep, timer);

    s->timed_out = true;
    wake_up(&s->wq);
}

/* timeout: tsc ticks, 0 not to wait, negative for ever. the count of ready fds, or -EINTR */
long poll_core(struct pollfd *fds, unsigned nfds, int64_t timeout) {
    struct poll_sleep s = { .timer = { .fn = poll_timeout } };
    struct poll_table pt;

    /* our own queue, and the two a fd may add */
    pt.max = 2 * nfds + 1;
    int order = SIZE_TO_ORDER(pt.max * sizeof(struct poll_entry));

    uintptr_t phys = (uintptr_t)alloc_pages(order);
    if (!phys) return -ENOMEM;

    pt.ents = (struct poll_entry *)P2V(phys);

    wait_queue_init(&s.wq);

    if (timeout > 0) {
        s.timer.expires = rdtsc() + timeout;
        timer_add(&s.timer);
    }

    long ret;

    for (;;) {
        pt.n = 0;
        poll_wait(&pt, &s.wq); /* the timer's wake_up lands here */

        unsigned count = poll_pass(fds, nfds, &pt);

        /* registered and asleep from the last of them on: a fd that became ready while the others were
         * being looked at is found now, and one that does it after wakes us */
        if (!count) count = poll_pass(fds, nfds, NULL);

        bool stop = count || timeout == 0 || s.timed_out || signal_pending(current_task());
        if (!stop) schedule();

        for (unsigned i = 0; i < pt.n; i++) finish_wait(pt.ents[i].wq, &pt.ents[i].we);

        if (count) {
            ret = count;
            break;
        }

        if (timeout == 0 || s.timed_out) {
            ret = 0;
            break;
        }

        if (signal_pending(current_task())) {
            ret = -EINTR; /* never restarted, whatever SA_RESTART says */
            break;
        }
    }

    if (timeout > 0) timer_del(&s.timer);

    free_pages((void *)phys, order);
    return ret;
}

int64_t ms_to_tsc(int64_t ms) {
    return ms * (int64_t)this_cpu_read(tsc_freq_hz) / 1000;
}

static long poll_user(long ufds, long nfds, int64_t timeout) {
    if (nfds < 0 || nfds > FILES_MAX) return -EINVAL;

    struct pollfd *fds = kmalloc(nfds ? nfds * sizeof(struct pollfd) : 1);
    if (!fds) return -ENOMEM;

    long ret = copy_from_user(fds, ufds, nfds * sizeof(struct pollfd));

    if (!ret) {
        ret = poll_core(fds, nfds, timeout);

        if (ret >= 0 && copy_to_user(ufds, fds, nfds * sizeof(struct pollfd)) != 0) ret = -EFAULT;
    }

    kfree(fds);
    return ret;
}

long sys_poll(long ufds, long nfds, long timeout_ms) {
    return poll_user(ufds, nfds, timeout_ms < 0 ? -1 : ms_to_tsc(timeout_ms));
}

/* a timespec as ticks to wait, -1 for none */
long timeout_from_timespec(long utsp, int64_t *timeout) {
    uint64_t t;

    *timeout = -1;
    if (!utsp) return 0;

    int err = timespec_to_tsc(utsp, &t);
    if (err) return err;

    *timeout = t > INT64_MAX ? INT64_MAX : (int64_t)t;
    return 0;
}

long sys_ppoll(long ufds, long nfds, long utsp, long usigmask, long sigsetsize) {
    int64_t timeout;

    long err = timeout_from_timespec(utsp, &timeout);
    if (err) return err;

    if (usigmask && (err = sigmask_swap(usigmask, sigsetsize))) return err;

    long ret = poll_user(ufds, nfds, timeout);

    if (usigmask) sigmask_unswap(ret);
    return ret;
}

/* the three sets as pollfds, and back. only the sets given are read and written */
static long select_user(long n, long ur, long uw, long ue, int64_t timeout) {
    uint64_t set[3][FD_SETSIZE / 64] = { { 0 } };
    const long user[3] = { ur, uw, ue };
    static const int16_t event[3] = { POLLIN, POLLOUT, POLLPRI };

    if (n < 0 || n > FD_SETSIZE) return -EINVAL;

    size_t bytes = (n + 7) / 8;

    for (int k = 0; k < 3; k++) {
        if (user[k] && copy_from_user(set[k], user[k], bytes) != 0) return -EFAULT;
    }

    struct pollfd *fds = kmalloc(FILES_MAX * sizeof(struct pollfd));
    if (!fds) return -ENOMEM;

    unsigned nfds = 0;
    long ret = 0;

    for (long fd = 0; fd < n && !ret; fd++) {
        int16_t events = 0;

        for (int k = 0; k < 3; k++)
            if (set[k][fd / 64] & (1ULL << (fd % 64))) events |= event[k];

        if (!events) continue;

        if (fd >= FILES_MAX) ret = -EBADF;
        else                 fds[nfds++] = (struct pollfd){ .fd = fd, .events = events };
    }

    if (!ret) ret = poll_core(fds, nfds, timeout);

    if (ret >= 0) {
        uint64_t out[3][FD_SETSIZE / 64] = { { 0 } };
        static const int16_t ready[3] = { POLLIN | POLLHUP | POLLERR, POLLOUT | POLLERR, POLLPRI };

        ret = 0;

        for (unsigned i = 0; i < nfds; i++) {
            if (fds[i].revents & POLLNVAL) {
                ret = -EBADF;
                break;
            }

            for (int k = 0; k < 3; k++) {
                if ((fds[i].events & event[k]) && (fds[i].revents & ready[k])) {
                    out[k][fds[i].fd / 64] |= 1ULL << (fds[i].fd % 64);
                    ret++;
                }
            }
        }

        for (int k = 0; ret >= 0 && k < 3; k++) {
            if (user[k] && copy_to_user(user[k], out[k], bytes) != 0) ret = -EFAULT;
        }
    }

    kfree(fds);
    return ret;
}

struct timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

/* the timeout is not written back, as linux does: portable programs set it again */
long sys_select(long n, long ur, long uw, long ue, long utv) {
    int64_t timeout = -1;

    if (utv) {
        struct timeval tv;

        if (copy_from_user(&tv, utv, sizeof(tv)) != 0) return -EFAULT;
        if (tv.tv_sec < 0 || tv.tv_usec < 0 || tv.tv_usec >= 1000000) return -EINVAL;

        uint64_t freq = this_cpu_read(tsc_freq_hz);
        timeout = tv.tv_sec * freq + tv.tv_usec * freq / 1000000;
    }

    return select_user(n, ur, uw, ue, timeout);
}

long sys_pselect6(long n, long ur, long uw, long ue, long utsp, long usig) {
    struct { uint64_t mask; uint64_t size; } sig = { 0, 0 };
    int64_t timeout;

    long err = timeout_from_timespec(utsp, &timeout);
    if (err) return err;

    if (usig && copy_from_user(&sig, usig, sizeof(sig)) != 0) return -EFAULT;
    if (sig.mask && (err = sigmask_swap(sig.mask, sig.size))) return err;

    long ret = select_user(n, ur, uw, ue, timeout);

    if (sig.mask) sigmask_unswap(ret);
    return ret;
}
