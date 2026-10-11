#include <serika/arch.h>
#include <serika/compiler.h>
#include <serika/sched.h>
#include <serika/cpu.h>
#include <serika/intc.h>
#include <serika/panic.h>
#include <serika/preempt.h>

#include <asm/arch.h>
#include <asm/trapframe.h>

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
    task->on_rq = true;
}

static void account_dequeue(struct runqueue *rq, struct task *task) {
    rq->load -= task->weight;
    rq->nr_running--;
    task->on_rq = false;
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

static void check_preempt_tick(struct runqueue *rq) {
    struct task *curr = rq->curr;

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

/* vruntime is only meaningful relative to its rq */
static void migrate_task(struct runqueue *src, struct runqueue *dst, struct task *task) {
    dequeue_task(src, task);
    account_dequeue(src, task);

    uint64_t lag = task->vruntime > src->min_vruntime ? task->vruntime - src->min_vruntime : 0;
    task->vruntime = dst->min_vruntime + lag;

    __atomic_store_n(&task->cpu, dst->cpu, 5); /* task_rq_lock() reads it unlocked */

    account_enqueue(dst, task);
    enqueue_task(dst, task);
}

/* pull one task from the busiest cpu. own lock held, irq off */
static void balance(struct runqueue *rq) {
    struct runqueue *busiest = NULL;

    /* no lock, a hint */
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        struct runqueue *r = &cpus[i].rq;

        if (r == rq || !r->online) continue;
        if (!busiest || r->nr_running > busiest->nr_running) busiest = r;
    }

    /* no swapping */
    if (!busiest || busiest->nr_running < rq->nr_running + 2) return;

    /* trylock: we hold ours already, the other side may be doing the same to us */
    if (!spin_trylock(&busiest->lock)) return;

    if (busiest->nr_running >= rq->nr_running + 2) {
        struct task *task;

        RB_FOREACH(task, task_tree, &busiest->tasks) {
            if (!task->pinned) break;
        }

        if (task) {
            migrate_task(busiest, rq, task);

            if (rq->curr == curcpu->idle) rq->need_resched = true;
        }
    }

    spin_unlock(&busiest->lock);
}

/* irq off */
void sched_tick(void) {
    struct cpu *c = curcpu;
    struct runqueue *rq = &c->rq;

    if (!rq->online) return; /* not initialized */

    spin_lock(&rq->lock);
    check_preempt_tick(rq);

    if (rq->curr == c->idle || c->ticks % SCHED_BALANCE_TICKS == 0) balance(rq);
    spin_unlock(&rq->lock);
}

void __schedule(bool preempt) {
    interrupt_status_t flags = arch_irq_save();

    struct cpu *c = curcpu;
    struct runqueue *rq = &c->rq;

    spin_lock(&rq->lock);

    struct task *prev = rq->curr;
    struct task *next;

    rq->need_resched = false;
    update_curr(rq);

    if (prev != c->idle) {
        /* preempted while sleeping stays: its condition is not checked yet */
        bool leaving = prev->state == TASK_DEAD || (prev->state == TASK_SLEEPING && !preempt);

        if (!leaving) {
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

    if (next != prev) {
        /* the count follows the task, we may resume on another cpu */
        prev->saved_preempt_count = preempt_count();
        preempt_count_set(next->saved_preempt_count);

        switch_to(prev, next);
    }

    /* new tasks never get here, task_start() / task_start_user() call it for them */
    finish_task_switch();
    arch_irq_restore(flags);
}

void schedule() {
    schedule_check((uintptr_t)__builtin_return_address(0));
    __schedule(false);
}

/* TODO */
void finish_task_switch(void) {
    struct runqueue *rq = &curcpu->rq; /* another cpu might have migrated us */
    struct task *dead = rq->dead;

    rq->dead = NULL;
    spin_unlock(&rq->lock);

    if (dead) task_dead(dead);
}

/* t->cpu changes only under the lock of the rq it leaves, so once held it stays */
static struct runqueue *task_rq_lock(struct task *t, uint64_t *flags) {
    for (;;) {
        struct runqueue *rq = &cpus[__atomic_load_n(&t->cpu, 5)].rq;

        *flags = spin_lock_irqsave(&rq->lock);
        if (__atomic_load_n(&t->cpu, 5) == rq->cpu) return rq;

        spin_unlock_irqrestore(&rq->lock, *flags);
    }
}

/* a sleeper gets half a latency of credit at most */
static void place_wakeup(struct runqueue *rq, struct task *t) {
    uint64_t credit = us_to_tsc(SCHED_LATENCY_US) / 2;
    uint64_t floor = rq->min_vruntime > credit ? rq->min_vruntime - credit : 0;

    if (t->vruntime < floor) t->vruntime = floor;
}

/* rq may be another cpu's, curr's vruntime is a tick old at most */
static bool wakeup_preempts(struct runqueue *rq, struct task *t) {
    struct task *curr = rq->curr;

    if (curr == cpus[rq->cpu].idle) return true;

    return t->vruntime + us_to_tsc(SCHED_WAKEUP_GRAN_US) < curr->vruntime;
}

bool wake_up_process(struct task *t) {
    uint64_t flags;
    struct runqueue *rq = task_rq_lock(t, &flags);

    bool woke = false;
    bool kick = false;

    if (t->state != TASK_SLEEPING) goto out;

    woke = true;

    /* still on the rq, between prepare_to_wait() and schedule(). schedule() keeps it */
    if (t->on_rq) {
        t->state = t == rq->curr ? TASK_RUNNING : TASK_RUNNABLE;
        goto out;
    }

    /* off the rq, so its context is saved */
    place_wakeup(rq, t);
    t->state = TASK_RUNNABLE;

    account_enqueue(rq, t);
    enqueue_task(rq, t);

    if (wakeup_preempts(rq, t)) {
        rq->need_resched = true;
        kick = rq->cpu != curcpu->id;
    }

out:
    spin_unlock_irqrestore(&rq->lock, flags);

    if (kick) intc_send_ipi(cpus[rq->cpu].hw_id, RESCHED_VECTOR);

    return woke;
}

/* sleeping and off its rq */
bool sched_task_sleeping(struct task *t) {
    uint64_t flags;
    struct runqueue *rq = task_rq_lock(t, &flags);

    bool asleep = t->state == TASK_SLEEPING && !t->on_rq;

    spin_unlock_irqrestore(&rq->lock, flags);

    return asleep;
}

bool sched_cpu_online(uint32_t cpu) {
    return __atomic_load_n(&cpus[cpu].rq.online, 5);
}

/* locked read */
uint32_t sched_nr_running(uint32_t cpu) {
    struct runqueue *rq = &cpus[cpu].rq;

    uint64_t flags = spin_lock_irqsave(&rq->lock);
    uint32_t nr = rq->nr_running;
    spin_unlock_irqrestore(&rq->lock, flags);

    return nr;
}

uint32_t sched_nr_online(void) {
    uint32_t n = 0;

    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        if (cpus[i].rq.online) n++;
    }

    return n;
}

/* no lock, only a hint */
static uint32_t select_cpu(void) {
    uint32_t best = curcpu->id;

    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        struct runqueue *rq = &cpus[i].rq;

        if (!rq->online) continue;
        if (rq->nr_running < cpus[best].rq.nr_running) best = i;
    }

    return best;
}

/* starting at 0 would starve everyone */
void sched_add_task_cpu(struct task *task, uint32_t cpu) {
    struct cpu *c = &cpus[cpu];
    struct runqueue *rq = &c->rq;

    uint64_t flags = spin_lock_irqsave(&rq->lock);

    task->vruntime = rq->min_vruntime;
    task->state = TASK_RUNNABLE;
    __atomic_store_n(&task->cpu, cpu, 5);

    account_enqueue(rq, task);
    enqueue_task(rq, task);

    bool kick = c != curcpu && rq->curr == c->idle;

    spin_unlock_irqrestore(&rq->lock, flags);

    if (kick) intc_send_ipi(c->hw_id, RESCHED_VECTOR); /* waits for next tick */
}

void sched_add_task(struct task *task) {
    sched_add_task_cpu(task, select_cpu());
}

notrace static void resched_handler(struct trapframe *tf, void *data) {
    (void)tf;
    (void)data;

    intc_eoi();
    preempt_schedule();
}

static void idle_loop(void) {
    for (;;) arch_halt();
}

/* every cpu, after cpu_init() */
void sched_init(void) {
    struct cpu *c = curcpu;
    struct runqueue *rq = &c->rq;

    spin_lock_init(&rq->lock);
    RB_INIT(&rq->tasks);
    rq->cpu = c->id;

    extern void register_handler(uint8_t vector, void (*handler)(struct trapframe *, void *), void *data);
    register_handler(RESCHED_VECTOR, resched_handler, NULL);

    c->idle = task_create(idle_loop, 19);
    if (!c->idle) panic("sched: unable to create idle task -- out of memory?");
    c->idle->cpu = c->id;

    struct task *boot = task_create_boot();
    if (!boot) panic("sched: unable to create boot task -- out of memory?");

    boot->vruntime = rq->min_vruntime;
    boot->pinned = true; /* runs on the boot stack */
    boot->cpu = c->id;
    boot->state = TASK_RUNNING;
    boot->exec_start = rdtsc();

    account_enqueue(rq, boot);
    rq->curr = boot;

    __atomic_store_n(&rq->online, true, 5); /* select_cpu() can pick us now */
}
