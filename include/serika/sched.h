#pragma once

#include <serika/task.h>

#include <deps/tree.h>

RB_HEAD(task_tree, task);

struct runqueue {
    struct task_tree tasks;
    struct task *curr;
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