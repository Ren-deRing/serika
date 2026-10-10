#include <serika/arch.h>
#include <serika/buddy.h>
#include <serika/compiler.h>
#include <serika/cpu.h>
#include <serika/page.h>
#include <serika/panic.h>
#include <serika/sched.h>
#include <serika/slab.h>
#include <serika/string.h>

static volatile uint64_t next_id = 0;

static struct task *task_alloc(task_entry_t entry, int nice) {
    struct task *task = kmalloc(sizeof(struct task));
    if (!task) return NULL;

    memset(task, 0, sizeof(*task));

    task->id     = __atomic_fetch_add(&next_id, 1, 5); /* __ATOMIC_SEQ_CST */
    task->entry  = entry;
    task->nice   = nice;
    task->weight = sched_prio_to_weight[nice + 20];
    task->pmap   = NULL; /* kthread, uses whatever cr3 is loaded */

    return task;
}

/* not runnable until sched_add_task() */
struct task *task_create(task_entry_t entry, int nice) {
    struct task *task = task_alloc(entry, nice);
    if (!task) return NULL;

    uintptr_t phys = (uintptr_t)alloc_pages(SIZE_TO_ORDER(TASK_KSTACK_SIZE));
    if (!phys) {
        kfree(task);
        return NULL;
    }

    task->kstack = (void *)P2V(phys);
    task->kernel_stack_size = TASK_KSTACK_SIZE;

    if (arch_kthread_init(task) == -1) {
        free_pages((void *)phys, SIZE_TO_ORDER(TASK_KSTACK_SIZE));
        kfree(task);
        return NULL;
    }

    return task;
}

struct task *task_create_boot(void) {
    struct task *task = task_alloc(NULL, 0);
    if (!task) return NULL;

    if (arch_xsaves_init(task) == -1) {
        kfree(task);
        return NULL;
    }

    return task;
}

void task_free(struct task *task) {
    if (task->kstack) free_pages((void *)V2P((uintptr_t)task->kstack), SIZE_TO_ORDER(task->kernel_stack_size));

    kfree(task->arch.xsaves_area);
    kfree(task);
}

/* schedule() switched to us holding the rq lock, irq off */
void task_start(void) {
    spin_unlock(&curcpu->rq.lock);
    arch_irq_enable();

    curcpu->rq.curr->entry();
    task_exit();
}

/* iretq will enable irq */
notrace void task_start_user(void) {
    spin_unlock(&curcpu->rq.lock);
}

void task_exit(void) {
    arch_irq_disable();

    curcpu->rq.curr->state = TASK_DEAD;
    schedule();

    panic("sched: dead task got scheduled -- how?");
}
