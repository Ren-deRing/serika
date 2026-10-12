#include <serika/cpu.h>
#include <serika/errno.h>
#include <serika/futex.h>
#include <serika/list.h>
#include <serika/lock.h>
#include <serika/mm.h>
#include <serika/preempt.h>
#include <serika/printk.h>
#include <serika/sched.h>
#include <serika/signal.h>
#include <serika/syscall.h>
#include <serika/timer.h>
#include <serika/wait.h>

#include <asm/arch.h>

#define FUTEX_WAIT            0
#define FUTEX_WAKE            1
#define FUTEX_REQUEUE         3
#define FUTEX_CMP_REQUEUE     4
#define FUTEX_WAIT_BITSET     9
#define FUTEX_WAKE_BITSET     10
#define FUTEX_PRIVATE_FLAG    128
#define FUTEX_CLOCK_REALTIME  256
#define FUTEX_CMD_MASK        (~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME))
#define FUTEX_BITSET_ANY      0xffffffffu

/* the key is (mm, address): no shared mappings yet, so private or not, the word is the mm's */
#define FUTEX_HASH 256

struct futex_bucket {
    spinlock_t lock;  /* a bucket lock, then a wait_queue lock, then an rq lock */
    list_node  waiters;
};

/* on the waiter's stack. anyone touching it holds the lock of the bucket it is in */
struct futex_q {
    list_node                    node;
    struct futex_bucket *volatile bucket; /* requeue moves it */
    struct mm                   *mm;
    uintptr_t                    addr;
    uint32_t                     bitset;

    struct wait_queue            wq;
    volatile bool                woken;
    volatile bool                timed_out;
    struct timer                 timer;
};

static struct futex_bucket buckets[FUTEX_HASH];

static struct futex_bucket *bucket_of(struct mm *mm, uintptr_t addr) {
    uint64_t h = ((uintptr_t)mm >> 6) ^ (addr >> 2);
    h ^= h >> 17;
    h *= 0x9e3779b97f4a7c15ULL;

    return &buckets[(h >> 32) % FUTEX_HASH];
}

/* the zeroed array is all unlocked: only the list heads need setting */
static void futex_init(void) {
    static volatile bool done;
    if (__atomic_load_n(&done, 2)) return; /* __ATOMIC_ACQUIRE */

    static spinlock_t init_lock = SPINLOCK_INITIALIZER;
    uint64_t flags = spin_lock_irqsave(&init_lock);

    if (!done) {
        for (int i = 0; i < FUTEX_HASH; i++) list_init(&buckets[i].waiters);
        __atomic_store_n(&done, true, 3); /* __ATOMIC_RELEASE */
    }

    spin_unlock_irqrestore(&init_lock, flags);
}

/* both buckets, in address order, once if they are the same */
static void lock_two(struct futex_bucket *a, struct futex_bucket *b, uint64_t *flags) {
    if (a > b) {
        struct futex_bucket *t = a;
        a = b;
        b = t;
    }

    *flags = spin_lock_irqsave(&a->lock);
    if (b != a) spin_lock(&b->lock);
}

static void unlock_two(struct futex_bucket *a, struct futex_bucket *b, uint64_t flags) {
    if (b != a) spin_unlock(a > b ? &a->lock : &b->lock);
    spin_unlock_irqrestore(a > b ? &b->lock : &a->lock, flags);
}

/* bucket lock held. q is gone from the list before it is woken */
static void wake_q(struct futex_q *q) {
    list_del(&q->node);
    q->woken = true;
    wake_up(&q->wq);
}

/* bucket lock held. n at most, those of (mm, addr) with a bit in common */
static int wake_locked(struct futex_bucket *b, struct mm *mm, uintptr_t addr, int n, uint32_t bitset) {
    int woke = 0;

    for (list_node *it = b->waiters.next; it != &b->waiters && woke < n;) {
        struct futex_q *q = container_of(it, struct futex_q, node);
        it = it->next;

        if (q->mm == mm && q->addr == addr && (q->bitset & bitset)) {
            wake_q(q);
            woke++;
        }
    }

    return woke;
}

int futex_wake(struct mm *mm, uintptr_t addr, int n) {
    futex_init();

    struct futex_bucket *b = bucket_of(mm, addr);

    uint64_t flags = spin_lock_irqsave(&b->lock);
    int woke = wake_locked(b, mm, addr, n, FUTEX_BITSET_ANY);
    spin_unlock_irqrestore(&b->lock, flags);

    return woke;
}

static void futex_timeout(struct timer *t) {
    struct futex_q *q = container_of(t, struct futex_q, timer);

    q->timed_out = true;
    wake_up(&q->wq);
}

/* deadline 0: none */
static long futex_wait(uintptr_t addr, uint32_t val, uint32_t bitset, uint64_t deadline) {
    struct mm *mm = current_task()->mm;
    struct futex_q q = { .mm = mm, .addr = addr, .bitset = bitset, .timer = { .expires = deadline, .fn = futex_timeout } };
    struct futex_bucket *b = bucket_of(mm, addr);
    uint32_t cur;
    uint64_t flags;

    if (!bitset) return -EINVAL;

    wait_queue_init(&q.wq);

    /* the word is checked under the bucket lock: a waker that changed it takes the same lock to wake */
    for (;;) {
        flags = spin_lock_irqsave(&b->lock);

        int err = mm_read_nofault(mm, addr, &cur);
        if (err != -EFAULT) {
            if (err) {
                spin_unlock_irqrestore(&b->lock, flags);
                return err;
            }
            break;
        }

        /* not in memory yet: bring it in, sleeping, then look again */
        spin_unlock_irqrestore(&b->lock, flags);
        if (copy_from_user(&cur, addr, sizeof(cur)) != 0) return -EFAULT;
    }

    if (cur != val) {
        spin_unlock_irqrestore(&b->lock, flags);
        return -EAGAIN;
    }

    q.bucket = b;
    list_add_tail(&q.node, &b->waiters);

    spin_unlock_irqrestore(&b->lock, flags);

    if (deadline) timer_add(&q.timer);

    int err = wait_event_interruptible(q.wq, q.woken || q.timed_out);

    if (deadline) timer_del(&q.timer);

    /* out of the list, under the lock of wherever a requeue put it. a waker done with q has let go of that lock */
    for (;;) {
        struct futex_bucket *at = q.bucket;
        flags = spin_lock_irqsave(&at->lock);

        if (at == q.bucket) {
            if (!q.woken) list_del(&q.node);
            spin_unlock_irqrestore(&at->lock, flags);
            break;
        }

        spin_unlock_irqrestore(&at->lock, flags);
    }

    if (q.woken) return 0;
    if (q.timed_out) return -ETIMEDOUT;
    return err ? -EINTR : 0;
}

/* wake nr_wake on addr, move up to nr_requeue of the rest to addr2. cmp: only if addr still holds val */
static long futex_requeue(uintptr_t addr, uintptr_t addr2, int nr_wake, int nr_requeue, bool cmp, uint32_t val) {
    struct mm *mm = current_task()->mm;
    struct futex_bucket *b1 = bucket_of(mm, addr), *b2 = bucket_of(mm, addr2);
    uint64_t flags;

    if (nr_wake < 0 || nr_requeue < 0) return -EINVAL;

    for (;;) {
        lock_two(b1, b2, &flags);

        uint32_t cur;
        int err = cmp ? mm_read_nofault(mm, addr, &cur) : 0;

        if (err != -EFAULT) {
            if (err || (cmp && cur != val)) {
                unlock_two(b1, b2, flags);
                return err ? err : -EAGAIN;
            }
            break;
        }

        unlock_two(b1, b2, flags);
        if (copy_from_user(&cur, addr, sizeof(cur)) != 0) return -EFAULT;
    }

    int woke = 0, moved = 0;

    for (list_node *it = b1->waiters.next; it != &b1->waiters;) {
        struct futex_q *q = container_of(it, struct futex_q, node);
        it = it->next;

        if (q->mm != mm || q->addr != addr) continue;

        if (woke < nr_wake) {
            wake_q(q);
            woke++;
        } else if (moved < nr_requeue) {
            list_del(&q->node);
            q->addr = addr2;
            q->bucket = b2;
            list_add_tail(&q->node, &b2->waiters);
            moved++;
        } else {
            break;
        }
    }

    unlock_two(b1, b2, flags);
    return woke + moved;
}

#define FUTEX_WAITERS     0x80000000u
#define FUTEX_OWNER_DIED  0x40000000u
#define FUTEX_TID_MASK    0x3fffffffu

#define ROBUST_LIMIT 2048 /* a list that loops is user's doing */

struct robust_list_head {
    uint64_t list;    /* circular, ends at the head itself */
    int64_t  futex_offset;
    uint64_t pending; /* being taken or dropped when it died */
};

long sys_set_robust_list(long head, long len) {
    if (len != sizeof(struct robust_list_head)) return -EINVAL;

    current_task()->robust_list = head;
    return 0;
}

/* a lock the dying task held: owner died, and someone waiting is woken. not atomic: the owner is gone, and a
 * waiter only sets a bit it keeps */
static void robust_death(struct mm *mm, uint64_t uaddr, uint32_t tid) {
    uint32_t uval;

    if (uaddr & 3 || copy_from_user(&uval, uaddr, sizeof(uval)) != 0) return;
    if ((uval & FUTEX_TID_MASK) != tid) return;

    uint32_t val = (uval & FUTEX_WAITERS) | FUTEX_OWNER_DIED;

    if (copy_to_user(uaddr, &val, sizeof(val)) != 0) return;
    if (uval & FUTEX_WAITERS) futex_wake(mm, uaddr, 1);
}

void futex_exit_robust(struct task *task) {
    struct robust_list_head head;
    uint32_t tid = task->id;

    if (!task->robust_list || copy_from_user(&head, task->robust_list, sizeof(head)) != 0) return;

    uint64_t entry = head.list;

    for (int n = 0; entry != task->robust_list && n < ROBUST_LIMIT; n++) {
        uint64_t next;

        if (copy_from_user(&next, entry, sizeof(next)) != 0) break;

        if (entry != head.pending) robust_death(task->mm, entry + head.futex_offset, tid);

        entry = next;
    }

    if (head.pending) robust_death(task->mm, head.pending + head.futex_offset, tid);

    task->robust_list = 0;
}

long sys_futex(long uaddr, long op, long val, long utime, long uaddr2, long val3) {
    int cmd = op & FUTEX_CMD_MASK;
    uint64_t deadline = 0, t;

    futex_init();

    if (uaddr & 3) return -EINVAL;

    switch (cmd) {
        case FUTEX_WAIT:
        case FUTEX_WAIT_BITSET:
            if (utime) {
                int err = timespec_to_tsc(utime, &t);
                if (err) return err;

                /* WAIT counts from now, WAIT_BITSET to a point on the clock: the monotonic one is the tsc, the
                 * realtime one is that plus the epoch at boot */
                deadline = cmd == FUTEX_WAIT ? rdtsc() + t : (op & FUTEX_CLOCK_REALTIME) ? realtime_to_tsc(t) : t;
                if (!deadline) deadline = 1;
            }

            return futex_wait(uaddr, val, cmd == FUTEX_WAIT ? FUTEX_BITSET_ANY : (uint32_t)val3, deadline);

        case FUTEX_WAKE:
        case FUTEX_WAKE_BITSET: {
            uint32_t bitset = cmd == FUTEX_WAKE ? FUTEX_BITSET_ANY : (uint32_t)val3;
            if (!bitset) return -EINVAL;

            struct mm *mm = current_task()->mm;
            struct futex_bucket *b = bucket_of(mm, uaddr);

            uint64_t flags = spin_lock_irqsave(&b->lock);
            int woke = wake_locked(b, mm, uaddr, val, bitset);
            spin_unlock_irqrestore(&b->lock, flags);

            return woke;
        }

        /* the count to requeue comes in the timeout's place */
        case FUTEX_REQUEUE:
            return futex_requeue(uaddr, uaddr2, val, utime, false, 0);

        case FUTEX_CMP_REQUEUE:
            return futex_requeue(uaddr, uaddr2, val, utime, true, val3);
    }

    printk("syscall: futex op %ld not supported -- task %lu\n", op, current_task()->id);
    return -ENOSYS;
}
