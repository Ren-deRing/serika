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
#include <serika/signal.h>
#include <serika/slab.h>
#include <serika/string.h>
#include <serika/syscall.h>
#include <serika/vfs.h>
#include <serika/wait.h>

static volatile uint64_t next_id = 0;

static spinlock_t tasklist_lock = SPINLOCK_INITIALIZER; /* parent, children, sibling, pid_hash. irqsave */

/* user tasks by id, for kill(). a task found under tasklist_lock is not freed before it is dropped */
#define PID_HASH 256

static struct task *pid_hash[PID_HASH];

/* tasklist_lock held */
static void pid_hash_del(struct task *task) {
    for (struct task **p = &pid_hash[task->id % PID_HASH]; *p; p = &(*p)->pid_next) {
        if (*p == task) {
            *p = task->pid_next;
            return;
        }
    }
}

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

/* a user task with its kernel stack. arch fills its first frame */
static struct task *user_task_alloc(uintptr_t entry, int nice) {
    int korder = SIZE_TO_ORDER(TASK_KSTACK_SIZE);

    struct task *task = task_alloc((task_entry_t)entry, nice);
    if (!task) return NULL;

    uintptr_t kstack_phys = (uintptr_t)alloc_pages(korder);
    if (!kstack_phys) {
        kfree(task);
        return NULL;
    }

    task->kstack = (void *)P2V(kstack_phys);
    task->kernel_stack_size = TASK_KSTACK_SIZE;

    return task;
}

/* one that never ran and owns nothing yet */
static void user_task_discard(struct task *task) {
    free_pages((void *)V2P((uintptr_t)task->kstack), SIZE_TO_ORDER(task->kernel_stack_size));
    kfree(task->arch.xsaves_area);
    kfree(task);
}

/* takes mm, files and sighand. the creator reaps it, kill() finds it */
static void user_task_attach(struct task *task, struct mm *mm, struct files *files, struct sighand *sighand) {
    task->mm = mm;
    task->pmap = mm->pmap;
    task->files = files;
    task->sighand = sighand;

    uint64_t f = spin_lock_irqsave(&tasklist_lock);
    task->parent = current_task();
    list_add_tail(&task->sibling, &task->parent->children);

    task->pid_next = pid_hash[task->id % PID_HASH];
    pid_hash[task->id % PID_HASH] = task;
    spin_unlock_irqrestore(&tasklist_lock, f);
}

/* not runnable until sched_add_task(). takes mm, files and sighand on success, the caller still owns them on failure */
struct task *task_create_mm(struct mm *mm, struct files *files, struct sighand *sighand,
                            uintptr_t entry, uintptr_t user_stack, int nice) {
    struct task *task = user_task_alloc(entry, nice);
    if (!task) return NULL;

    if (arch_task_init(task, user_stack) == -1) {
        user_task_discard(task);
        return NULL;
    }

    user_task_attach(task, mm, files, sighand);
    return task;
}

/* clone without CLONE_VM: a copy of the caller that returns 0 from the same system call. not runnable yet */
int task_fork(const struct trapframe *tf, struct task **out) {
    struct task *curr = current_task();

    struct task *task = user_task_alloc(0, curr->nice);
    if (!task) return -ENOMEM;

    struct mm *mm = mm_fork(curr->mm);
    struct files *files = files_copy(curr->files);
    struct sighand *sighand = sighand_copy(curr->sighand);

    if (!mm || !files || !sighand || arch_fork_init(task, tf) == -1) {
        if (mm) mm_put(mm);
        if (files) files_put(files);
        if (sighand) sighand_put(sighand);
        user_task_discard(task);
        return -ENOMEM;
    }

    task->blocked = curr->blocked;

    user_task_attach(task, mm, files, sighand);

    *out = task;
    return 0;
}

/* execve: the task runs on mm from here. the old one goes once this cpu has left it */
void task_exec_mm(struct mm *mm) {
    struct task *curr = current_task();
    struct mm *old = curr->mm;

    interrupt_status_t flags = arch_irq_save();

    pmap_switch(curr->pmap, mm->pmap);
    curr->mm = mm;
    curr->pmap = mm->pmap;
    pmap_activate(mm->pmap);

    arch_irq_restore(flags);

    mm_put(old);
}

/* not runnable until sched_add_task() */
struct task *task_create_user(const void *code, size_t size, int nice) {
    struct mm *mm = mm_create();
    struct files *files = files_create();
    struct sighand *sighand = sighand_create();

    if (!mm || !files || !sighand) goto fail;

    if (mm_map(mm, USER_CODE_BASE, ALIGN_UP(size, PAGE_SIZE), PROT_READ | PROT_EXEC) != 0) goto fail;
    if (mm_map(mm, USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_SIZE, PROT_READ | PROT_WRITE) != 0) goto fail;
    if (mm_copy_to(mm, USER_CODE_BASE, code, size) != 0) goto fail;

    struct task *task = task_create_mm(mm, files, sighand, USER_CODE_BASE, USER_STACK_TOP, nice);
    if (!task) goto fail;

    return task;

fail:
    if (mm) mm_put(mm);
    if (files) files_put(files);
    if (sighand) sighand_put(sighand);
    return NULL;
}

/* not runnable until sched_add_task(). shares proc's mm, user_stack is in a vma the caller mapped */
struct task *task_create_thread(struct task *proc, uintptr_t entry, uintptr_t user_stack, int nice) {
    if (!proc->mm) return NULL;

    mm_get(proc->mm);
    files_get(proc->files);
    sighand_get(proc->sighand);

    struct task *task = task_create_mm(proc->mm, proc->files, proc->sighand, entry, user_stack, nice);
    if (!task) {
        mm_put(proc->mm);
        files_put(proc->files);
        sighand_put(proc->sighand);
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

/* a task that ran dropped its files in task_exit() */
void task_free(struct task *task) {
    /* kill() can't find it from here on */
    if (task->sighand) {
        uint64_t f = spin_lock_irqsave(&tasklist_lock);
        pid_hash_del(task);
        spin_unlock_irqrestore(&tasklist_lock, f);

        sighand_put(task->sighand);
    }

    if (task->mm) mm_put(task->mm);
    if (task->files) files_put(task->files);

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
        struct ksiginfo info = { .signo = SIGCHLD, .code = CLD_EXITED, .pid = task->id };

        task->state = TASK_ZOMBIE;
        wake_up(&parent->wait_chldexit);
        send_signal(parent, SIGCHLD, &info); /* a kthread parent takes none */
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
static long reap_child(struct task *curr, long pid, long *status) {
    struct task *zombie = NULL;
    bool found = false;

    uint64_t f = spin_lock_irqsave(&tasklist_lock);

    for (list_node *n = curr->children.next; n != &curr->children; n = n->next) {
        struct task *c = container_of(n, struct task, sibling);
        if (pid != -1 && (long)c->id != pid) continue;

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

/* pid -1: any child */
long task_waitpid(long pid, long *status, int options) {
    struct task *curr = current_task();
    long ret;

    if (options & WNOHANG) return reap_child(curr, pid, status);

    /* a signal ends the wait: -ERESTARTSYS, which the way back to user turns into a restart or -EINTR */
    int err = wait_event_interruptible(curr->wait_chldexit, (ret = reap_child(curr, pid, status)) != 0);
    return err ? err : ret;
}

/* child NULL: any child */
long task_wait(struct task *child, long *status, int options) {
    return task_waitpid(child ? (long)child->id : -1, status, options);
}

int task_signal(long pid, int sig, const struct ksiginfo *info) {
    int err = -ESRCH;

    uint64_t f = spin_lock_irqsave(&tasklist_lock);

    for (struct task *t = pid_hash[pid % PID_HASH]; t; t = t->pid_next) {
        if ((long)t->id == pid) {
            err = sig ? send_signal(t, sig, info) : 0;
            break;
        }
    }

    spin_unlock_irqrestore(&tasklist_lock, f);
    return err;
}

void task_exit(long code) {
    struct task *curr = current_task();

    /* while we still run: task_free() may come from finish_task_switch(), irq off */
    if (curr->files) {
        files_put(curr->files);
        curr->files = NULL;
    }

    exit_notify(curr);

    arch_irq_disable();

    curr->exit_code = code;
    curr->state = TASK_DEAD;
    schedule();

    panic("sched: dead task got scheduled -- how?");
}
