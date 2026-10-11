#include <serika/arch.h>
#include <serika/buddy.h>
#include <serika/compiler.h>
#include <serika/cpu.h>
#include <serika/errno.h>
#include <serika/mm.h>
#include <serika/page.h>
#include <serika/panic.h>
#include <serika/pmap.h>
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
    task->pmap   = pmap_kernel(); /* kthread. user tasks replace it */

    /* first run drops the rq lock of whoever switched to us */
    task->saved_preempt_count = 1;

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

/* not runnable until sched_add_task() */
struct task *task_create_user(const void *code, size_t size, int nice) {
    struct task *task = task_alloc((task_entry_t)USER_CODE_BASE, nice);
    if (!task) return NULL;

    int korder = SIZE_TO_ORDER(TASK_KSTACK_SIZE);

    struct mm *mm = mm_create();
    uintptr_t kstack_phys = (uintptr_t)alloc_pages(korder);

    if (!mm || !kstack_phys) goto fail;

    if (mm_map(mm, USER_CODE_BASE, ALIGN_UP(size, PAGE_SIZE), PROT_READ | PROT_EXEC) != 0) goto fail;
    if (mm_map(mm, USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_SIZE, PROT_READ | PROT_WRITE) != 0) goto fail;
    if (mm_copy_to(mm, USER_CODE_BASE, code, size) != 0) goto fail;

    task->mm = mm;
    task->pmap = mm->pmap;
    task->parent = current_task(); /* creator reaps */
    task->kstack = (void *)P2V(kstack_phys);
    task->kernel_stack_size = TASK_KSTACK_SIZE;

    if (arch_task_init(task) == -1) goto fail;

    return task;

fail:
    if (mm) mm_put(mm);
    free_pages((void *)kstack_phys, korder);
    kfree(task);
    return NULL;
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
    if (task->mm) mm_put(task->mm);

    if (task->kstack) free_pages((void *)V2P((uintptr_t)task->kstack), SIZE_TO_ORDER(task->kernel_stack_size));

    kfree(task->arch.xsaves_area);
    kfree(task);
}

/* rq lock held, irq off */
void task_start(void) {
    finish_task_switch();
    arch_irq_enable();

    current_task()->entry();
    task_exit(0);
}

/* iretq will enable irq */
notrace void task_start_user(void) {
    finish_task_switch();
    arch_user_gs_init();
}

long task_wait(struct task *child, long *status) {
    if (child->parent != current_task()) return -ECHILD;
    if (__atomic_load_n(&child->state, 2) != TASK_ZOMBIE) return 0; /* __ATOMIC_ACQUIRE */

    long id = child->id;
    *status = child->exit_code;

    task_free(child);
    return id;
}

void task_exit(long code) {
    arch_irq_disable();

    struct task *curr = current_task();

    curr->exit_code = code;
    curr->state = TASK_DEAD;
    schedule();

    panic("sched: dead task got scheduled -- how?");
}
