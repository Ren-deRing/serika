#pragma once

#include <serika/task.h>

#include <deps/tree.h>
#include <stdbool.h>

#define NICE_0_WEIGHT     1024
#define SCHED_LATENCY_US  6000 /* every task runs once in here */
#define SCHED_MIN_GRAN_US 750  /* ...unless there are too many of them */

RB_HEAD(task_tree, task);

struct runqueue {
    struct task_tree tasks; /* curr is not in here */
    struct task *curr;
    struct task *dead;      /* freed by next schedule() */

    uint64_t min_vruntime;
    uint64_t load;          /* sum of weights, curr included */
    uint32_t nr_running;    /* curr included, idle not */

    bool     need_resched;
};

static inline int task_cmp(struct task *a, struct task *b) {
    if (a->vruntime < b->vruntime)
        return (-1);
    if (a->vruntime > b->vruntime)
        return (1);

    if (a->id < b->id)
        return (-1);
    if (a->id > b->id)
        return (1);

    return (0);
}

RB_PROTOTYPE(task_tree, task, sched_entry, task_cmp);

void enqueue_task(struct runqueue *rq, struct task *task);
void dequeue_task(struct runqueue *rq, struct task *task);

struct task *pick_next_task(struct runqueue *rq);

void schedule();

void sched_init(void);
void sched_tick(void);
void sched_add_task(struct task *task);

extern const uint32_t sched_prio_to_weight[40];
