#pragma once

#include <serika/list.h>
#include <serika/lock.h>
#include <serika/preempt.h>
#include <serika/sched.h>

/* lock order: wait_queue->lock, then rq->lock. */

struct wait_queue {
    spinlock_t lock;
    list_node  head;
};

/* lives on the waiter's stack. node.next == NULL: not queued */
struct wait_entry {
    list_node    node;
    struct task *task;
};

void wait_queue_init(struct wait_queue *wq);

void prepare_to_wait(struct wait_queue *wq, struct wait_entry *we);
void finish_wait(struct wait_queue *wq, struct wait_entry *we);

void wake_up(struct wait_queue *wq); /* wakes every waiter */

/* sleeping before the check */
#define wait_event(wq, cond)                    \
    do {                                        \
        struct wait_entry __we = { 0 };         \
                                                \
        might_sleep();                          \
                                                \
        for (;;) {                              \
            prepare_to_wait(&(wq), &__we);      \
            if (cond) break;                    \
            schedule();                         \
        }                                       \
                                                \
        finish_wait(&(wq), &__we);              \
    } while (0)
