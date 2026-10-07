#include <serika/arch.h>
#include <serika/sched.h>
#include <serika/cpu.h>
#include <serika/panic.h>

#include <asm/arch.h>

RB_PROTOTYPE(task_tree, task, sched_entry, task_cmp);
RB_GENERATE(task_tree, task, sched_entry, task_cmp);

const uint32_t sched_prio_to_weight[40] = {
    /* -20 */ 88761, 71755, 56483, 46273, 36291,
    /* -15 */ 29154, 23254, 18705, 14949, 11916,
    /* -10 */  9548,  7620,  6100,  4904,  3906,
    /*  -5 */  3121,  2501,  1991,  1586,  1277,
    /*   0 */  1024,   820,   655,   526,   423,
    /*   5 */   335,   272,   215,   172,   137,
    /*  10 */   110,    87,    70,    56,    45,
    /*  15 */    36,    29,    23,    18,    15,
};

/* every time value in here is tsc ticks, not ns. */
static inline uint64_t us_to_tsc(uint64_t us) {
    return curcpu->tsc_freq_hz / 1000000ULL * us;
}

struct task *pick_next_task(struct runqueue *rq) {
    return RB_MIN(task_tree, &rq->tasks);
}

void enqueue_task(struct runqueue *rq, struct task *task) {
    RB_INSERT(task_tree, &rq->tasks, task);
}

void dequeue_task(struct runqueue *rq, struct task *task) {
    RB_REMOVE(task_tree, &rq->tasks, task);
}

static void account_enqueue(struct runqueue *rq, struct task *task) {
    rq->load += task->weight;
    rq->nr_running++;
}

static void account_dequeue(struct runqueue *rq, struct task *task) {
    rq->load -= task->weight;
    rq->nr_running--;
}

static void update_min_vruntime(struct runqueue *rq) {
    struct task *curr = rq->curr;
    struct task *left = pick_next_task(rq);
    uint64_t vruntime = rq->min_vruntime;

    bool curr_counts = curr != curcpu->idle && curr->state == TASK_RUNNING;

    if (curr_counts) vruntime = curr->vruntime;

    if (left) {
        if (!curr_counts || left->vruntime < vruntime) vruntime = left->vruntime;
    }

    /* never goes backwards */
    if (vruntime > rq->min_vruntime) rq->min_vruntime = vruntime;
}

static void update_curr(struct runqueue *rq) {
    struct task *curr = rq->curr;
    uint64_t now = rdtsc();
    uint64_t delta = now - curr->exec_start;

    curr->exec_start = now;

    if (curr == curcpu->idle) return;

    curr->sum_exec += delta;
    curr->vruntime += delta * NICE_0_WEIGHT / curr->weight; /* heavier task -> slower clock */

    update_min_vruntime(rq);
}

static uint64_t sched_slice(struct runqueue *rq, struct task *task) {
    uint64_t period = us_to_tsc(SCHED_LATENCY_US);
    uint64_t min_gran = us_to_tsc(SCHED_MIN_GRAN_US);

    if (rq->nr_running > period / min_gran) period = rq->nr_running * min_gran;

    return period * task->weight / rq->load;
}

/* irq off */
void sched_tick(void) {
    struct runqueue *rq = &curcpu->rq;
    struct task *curr = rq->curr;

    if (!curr) return; /* not initialized */

    update_curr(rq);

    if (curr == curcpu->idle) {
        if (rq->nr_running) rq->need_resched = true;
        return;
    }

    uint64_t ran = curr->sum_exec - curr->prev_sum_exec;
    uint64_t slice = sched_slice(rq, curr);

    if (ran > slice) { rq->need_resched = true; return; }
    if (ran < us_to_tsc(SCHED_MIN_GRAN_US)) return;

    struct task *left = pick_next_task(rq);
    if (left && curr->vruntime > left->vruntime && curr->vruntime - left->vruntime > slice) rq->need_resched = true;
}

void schedule() {
    interrupt_status_t flags = arch_irq_save();

    struct cpu *c = curcpu;
    struct runqueue *rq = &c->rq;
    struct task *prev = rq->curr;
    struct task *next;

    /* prev of last switch died.*/
    if (rq->dead) {
        task_free(rq->dead);
        rq->dead = NULL;
    }

    rq->need_resched = false;
    update_curr(rq);

    if (prev != c->idle) {
        if (prev->state == TASK_RUNNING) {
            prev->state = TASK_RUNNABLE;
            enqueue_task(rq, prev);
        } else {
            account_dequeue(rq, prev);
            if (prev->state == TASK_DEAD) rq->dead = prev;
        }
    }

    next = pick_next_task(rq);

    if (next) dequeue_task(rq, next);
    else      next = c->idle;

    next->state = TASK_RUNNING;
    next->exec_start = rdtsc();
    next->prev_sum_exec = next->sum_exec;

    rq->curr = next;

    if (next != prev) switch_to(prev, next);

    arch_irq_restore(flags);
}

/* starting at 0 would starve everyone */
void sched_add_task(struct task *task) {
    interrupt_status_t flags = arch_irq_save();
    struct runqueue *rq = &curcpu->rq;

    task->vruntime = rq->min_vruntime;
    task->state = TASK_RUNNABLE;

    account_enqueue(rq, task);
    enqueue_task(rq, task);

    arch_irq_restore(flags);
}

static void idle_loop(void) {
    for (;;) arch_halt();
}

void sched_init(void) {
    struct cpu *c = curcpu;
    struct runqueue *rq = &c->rq;

    RB_INIT(&rq->tasks);

    c->idle = task_create(idle_loop, 19);
    if (!c->idle) panic("sched: unable to create idle task -- out of memory?");

    struct task *boot = task_create_boot();
    if (!boot) panic("sched: unable to create boot task -- out of memory?");

    boot->vruntime = rq->min_vruntime;
    boot->state = TASK_RUNNING;
    boot->exec_start = rdtsc();

    account_enqueue(rq, boot);
    rq->curr = boot;
}
