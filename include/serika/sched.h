#pragma once

#include <serika/task.h>
#include <serika/lock.h>

#include <deps/tree.h>
#include <stdbool.h>

#define NICE_0_WEIGHT        1024
#define SCHED_LATENCY_US     6000 /* every task runs once in here */
#define SCHED_MIN_GRAN_US    750  /* ...unless there are too many of them */
#define SCHED_WAKEUP_GRAN_US 1000 /* a woken task preempts if this much behind curr */

#define SCHED_BALANCE_TICKS  4 /* balances every this ticks. for idle cpu, every tick */

#define RESCHED_VECTOR       0x21

RB_HEAD(task_tree, task);

struct runqueue {
    spinlock_t lock;        /* held across switch_to. next task releases */
    uint32_t   cpu;

    struct task_tree tasks; /* curr is not in here */
    struct task *curr;
    struct task *dead;      /* finish_task_switch() frees or zombifies it */

    uint64_t min_vruntime;
    uint64_t load;          /* sum of weights, curr included */
    uint32_t nr_running;    /* curr included, idle not */

    bool     need_resched;
    volatile bool online;
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
void __schedule(bool preempt); /* sched.c, preempt.c */
void preempt_schedule(void);
void finish_task_switch(void);

/* never with an rq lock held */
bool wake_up_process(struct task *t);

void sched_init(void);
void sched_tick(void);
void sched_add_task(struct task *task);
void sched_add_task_cpu(struct task *task, uint32_t cpu);
uint32_t sched_nr_online(void);
bool sched_cpu_online(uint32_t cpu);
uint32_t sched_nr_running(uint32_t cpu);
bool sched_task_sleeping(struct task *t);

extern const uint32_t sched_prio_to_weight[40];
