#include <serika/sched.h>
#include <serika/cpu.h>

RB_PROTOTYPE(task_tree, task, sched_entry, task_cmp);
RB_GENERATE(task_tree, task, sched_entry, task_cmp);

struct task *pick_next_task(struct runqueue *rq) {
    return RB_MIN(task_tree, &rq->tasks);
}

void enqueue_task(struct runqueue *rq, struct task *task) {
    RB_INSERT(task_tree, &rq->tasks, task);
}

void dequeue_task(struct runqueue *rq, struct task *task) {
    RB_REMOVE(task_tree, &rq->tasks, task);
}

extern void switch_to(struct task *curr, struct task *next);
void schedule() {
    struct runqueue *rq = &curcpu->rq;
    struct task *curr = rq->curr;
    struct task *next;

    dequeue_task(rq, curr);

    next = pick_next_task(rq);

    if (next == NULL)
        return;

    rq->curr = next;

    if (next != curr)
        switch_to(curr, next);
}
