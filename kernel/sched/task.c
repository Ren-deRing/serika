#include <serika/arch.h>
#include <serika/buddy.h>
#include <serika/compiler.h>
#include <serika/cpu.h>
#include <serika/errno.h>
#include <serika/mm.h>
#include <serika/page.h>
#include <serika/panic.h>
#include <serika/pmap.h>
#include <serika/preempt.h>
#include <serika/sched.h>
#include <serika/slab.h>
#include <serika/string.h>
#include <serika/syscall.h>
#include <serika/wait.h>

static volatile uint64_t next_id = 0;

static spinlock_t tasklist_lock = SPINLOCK_INITIALIZER; /* parent, children, sibling. irqsave */

static struct task *task_alloc(task_entry_t entry, int nice) {
    struct task *task = kmalloc(sizeof(struct task));
    if (!task) return NULL;

    memset(task, 0, sizeof(*task));

    task->id     = __atomic_fetch_add(&next_id, 1, 5); /* __ATOMIC_SEQ_CST */
    task->entry  = entry;
    task->nice   = nice;
    task->weight = sched_prio_to_weight[nice + 20];
    task->pmap   = pmap_kernel(); /* kthread. user tasks replace it */

    list_init(&task->children);
    wait_queue_init(&task->wait_chldexit);

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
    task->kstack = (void *)P2V(kstack_phys);
    task->kernel_stack_size = TASK_KSTACK_SIZE;

    if (arch_task_init(task) == -1) goto fail;

    /* creator reaps */
    uint64_t f = spin_lock_irqsave(&tasklist_lock);
    task->parent = current_task();
    list_add_tail(&task->sibling, &task->parent->children);
    spin_unlock_irqrestore(&tasklist_lock, f);

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

/* finish_task_switch(), after the rq lock is dropped */
void task_dead(struct task *task) {
    uint64_t f = spin_lock_irqsave(&tasklist_lock);

    struct task *parent = task->parent;

    if (parent) {
        task->state = TASK_ZOMBIE;
        wake_up(&parent->wait_chldexit);
    }

    spin_unlock_irqrestore(&tasklist_lock, f);

    if (!parent) task_free(task);
}

/* children lose their parent. */
static void exit_notify(struct task *curr) {
    list_node zombies;
    list_init(&zombies);

    uint64_t f = spin_lock_irqsave(&tasklist_lock);

    while (!list_empty(&curr->children)) {
        struct task *child = list_first(&curr->children, struct task, sibling);

        list_del(&child->sibling);
        child->parent = NULL;

        if (child->state == TASK_ZOMBIE) list_add_tail(&child->sibling, &zombies);
    }

    spin_unlock_irqrestore(&tasklist_lock, f);

    while (!list_empty(&zombies)) {
        struct task *child = list_first(&zombies, struct task, sibling);

        list_del(&child->sibling);
        task_free(child);
    }
}

/* 0: not exited yet, -ECHILD: no such child */
static long reap_child(struct task *curr, struct task *child, long *status) {
    struct task *zombie = NULL;
    bool found = false;

    uint64_t f = spin_lock_irqsave(&tasklist_lock);

    for (list_node *n = curr->children.next; n != &curr->children; n = n->next) {
        struct task *c = container_of(n, struct task, sibling);
        if (child && c != child) continue;

        found = true;

        if (c->state == TASK_ZOMBIE) {
            zombie = c;
            list_del(&c->sibling);
            break;
        }
    }

    spin_unlock_irqrestore(&tasklist_lock, f);

    if (!zombie) return found ? 0 : -ECHILD;

    long id = zombie->id;
    if (status) *status = zombie->exit_code;

    task_free(zombie);
    return id;
}

/* child NULL: any child */
long task_wait(struct task *child, long *status, int options) {
    struct task *curr = current_task();
    long ret;

    if (options & WNOHANG) return reap_child(curr, child, status);

    wait_event(curr->wait_chldexit, (ret = reap_child(curr, child, status)) != 0);
    return ret;
}

void task_exit(long code) {
    struct task *curr = current_task();

    exit_notify(curr);

    arch_irq_disable();

    curr->exit_code = code;
    curr->state = TASK_DEAD;
    schedule();

    panic("sched: dead task got scheduled -- how?");
}
