#include <serika/arch.h>
#include <serika/buddy.h>
#include <serika/compiler.h>
#include <serika/cpu.h>
#include <serika/errno.h>
#include <serika/futex.h>
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

/* a process of its own, led by leader */
static struct tgroup *tgroup_alloc(struct task *leader) {
    struct tgroup *g = kmalloc(sizeof(struct tgroup));
    if (!g) return NULL;

    *g = (struct tgroup){ .users = 1, .tgid = leader->id, .pgid = leader->id, .sid = leader->id, .live = 1, .leader = leader };
    list_init(&g->threads);

    return g;
}

/* tasklist_lock held */
static void pid_hash_add(struct task *task) {
    task->pid_next = pid_hash[task->id % PID_HASH];
    pid_hash[task->id % PID_HASH] = task;
}

/* a new process: takes mm, files, sighand and g. a child of the caller's process, which reaps it */
static void user_task_attach(struct task *task, struct tgroup *g, struct mm *mm, struct files *files,
                             struct sighand *sighand) {
    struct task *curr = current_task();

    task->mm = mm;
    task->pmap = mm->pmap;
    task->files = files;
    task->sighand = sighand;
    task->group = g;

    uint64_t f = spin_lock_irqsave(&tasklist_lock);

    /* forked by any thread, it is the process's: the leader holds the children */
    task->parent = curr->group ? curr->group->leader : curr;
    list_add_tail(&task->sibling, &task->parent->children);

    list_add_tail(&task->group_node, &g->threads);
    pid_hash_add(task);

    spin_unlock_irqrestore(&tasklist_lock, f);
}

/* a thread of g's process: takes mm, files and sighand. nobody reaps it, it frees itself. false once the process is going */
static bool thread_attach(struct task *task, struct tgroup *g, struct mm *mm, struct files *files,
                          struct sighand *sighand) {
    task->mm = mm;
    task->pmap = mm->pmap;
    task->files = files;
    task->sighand = sighand;

    uint64_t f = spin_lock_irqsave(&tasklist_lock);

    bool ok = !g->exiting && g->live;

    if (ok) {
        __atomic_add_fetch(&g->users, 1, 0); /* __ATOMIC_RELAXED */
        g->live++;

        task->group = g;
        list_add_tail(&task->group_node, &g->threads);
        pid_hash_add(task);
    }

    spin_unlock_irqrestore(&tasklist_lock, f);

    if (!ok) {
        task->mm = NULL;
        task->files = NULL;
        task->sighand = NULL;
    }

    return ok;
}

/* not runnable until sched_add_task(). takes mm, files and sighand on success, the caller still owns them on failure */
struct task *task_create_mm(struct mm *mm, struct files *files, struct sighand *sighand,
                            uintptr_t entry, uintptr_t user_stack, int nice) {
    struct task *task = user_task_alloc(entry, nice);
    if (!task) return NULL;

    struct tgroup *g = tgroup_alloc(task);

    if (!g || arch_task_init(task, user_stack) == -1) {
        kfree(g);
        user_task_discard(task);
        return NULL;
    }

    user_task_attach(task, g, mm, files, sighand);
    return task;
}

struct vfork_done {
    struct wait_queue wq;
    volatile bool     done;
    uint32_t          refs; /* the parent's and the child's. atomic */
};

struct vfork_done *vfork_alloc(void) {
    struct vfork_done *vd = kmalloc(sizeof(struct vfork_done));
    if (!vd) return NULL;

    wait_queue_init(&vd->wq);
    vd->done = false;
    vd->refs = 2;

    return vd;
}

static void vfork_put(struct vfork_done *vd) {
    if (__atomic_sub_fetch(&vd->refs, 1, 4) == 0) kfree(vd); /* __ATOMIC_ACQ_REL */
}

void vfork_attach(struct task *child, struct vfork_done *vd) {
    child->vfork_done = vd;
}

/* the child never ran */
void vfork_cancel(struct vfork_done *vd) {
    kfree(vd);
}

void vfork_wait(struct vfork_done *vd) {
    wait_event(vd->wq, vd->done);
    vfork_put(vd);
}

/* the child is out of the parent's mm: exec has its own, or it is gone */
static void vfork_release(struct task *task) {
    struct vfork_done *vd = task->vfork_done;
    if (!vd) return;

    task->vfork_done = NULL;
    vd->done = true;
    wake_up(&vd->wq);
    vfork_put(vd);
}

/* clone without CLONE_THREAD: a copy of the caller that returns 0 from the same system call. not runnable yet.
 * share_mm: the same address space, not a copy of it, for a parent that waits. clear_sighand: no handlers */
int task_fork(const struct trapframe *tf, bool share_mm, bool clear_sighand, struct task **out) {
    struct task *curr = current_task();

    struct task *task = user_task_alloc(0, curr->nice);
    if (!task) return -ENOMEM;

    struct mm *mm = share_mm ? curr->mm : mm_fork(curr->mm);
    if (share_mm) mm_get(mm);

    struct files *files = files_copy(curr->files);
    struct sighand *sighand = sighand_copy(curr->sighand);
    struct tgroup *g = tgroup_alloc(task);

    if (!mm || !files || !sighand || !g || arch_fork_init(task, tf) == -1) {
        if (mm) mm_put(mm);
        if (files) files_put(files);
        if (sighand) sighand_put(sighand);
        kfree(g);
        user_task_discard(task);
        return -ENOMEM;
    }

    if (clear_sighand) sighand_reset(sighand);

    task->blocked = curr->blocked;

    /* same group and session until it says otherwise. the caller's is read under the lock: setpgid() */
    if (curr->group) {
        uint64_t f = spin_lock_irqsave(&tasklist_lock);
        g->pgid = curr->group->pgid;
        g->sid = curr->group->sid;
        spin_unlock_irqrestore(&tasklist_lock, f);
    }

    user_task_attach(task, g, mm, files, sighand);

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
    vfork_release(curr); /* the parent has its mm to itself again */
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

/* not runnable until sched_add_task(). a thread of proc's process, user_stack in a vma the caller mapped */
struct task *task_create_thread(struct task *proc, uintptr_t entry, uintptr_t user_stack, int nice) {
    if (!proc->group) return NULL;

    struct task *task = user_task_alloc(entry, nice);
    if (!task) return NULL;

    if (arch_task_init(task, user_stack) == -1) {
        user_task_discard(task);
        return NULL;
    }

    mm_get(proc->mm);
    files_get(proc->files);
    sighand_get(proc->sighand);

    if (!thread_attach(task, proc->group, proc->mm, proc->files, proc->sighand)) {
        mm_put(proc->mm);
        files_put(proc->files);
        sighand_put(proc->sighand);
        user_task_discard(task);
        return NULL;
    }

    return task;
}

/* clone with CLONE_THREAD: the caller's process gets another thread, back from the same system call with 0 */
int task_clone_thread(const struct trapframe *tf, struct task **out) {
    struct task *curr = current_task();

    struct task *task = user_task_alloc(0, curr->nice);
    if (!task) return -ENOMEM;

    if (arch_fork_init(task, tf) == -1) {
        user_task_discard(task);
        return -ENOMEM;
    }

    mm_get(curr->mm);
    files_get(curr->files);
    sighand_get(curr->sighand);

    task->blocked = curr->blocked;

    if (!thread_attach(task, curr->group, curr->mm, curr->files, curr->sighand)) {
        mm_put(curr->mm);
        files_put(curr->files);
        sighand_put(curr->sighand);
        user_task_discard(task);
        return -EAGAIN;
    }

    *out = task;
    return 0;
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
    if (task->group && __atomic_sub_fetch(&task->group->users, 1, 4) == 0) {   /* __ATOMIC_ACQ_REL */
        timer_del(&task->group->itimer);
        kfree(task->group);
    }

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

/* tasklist_lock held. its children go to no one: zombies among them, done, go on zombies to be freed */
static void orphan_children(struct task *task, list_node *zombies) {
    while (!list_empty(&task->children)) {
        struct task *child = list_first(&task->children, struct task, sibling);

        list_del(&child->sibling);
        child->parent = NULL;

        /* a leader whose threads still run is freed by the last of them */
        if (child->state == TASK_ZOMBIE) list_add_tail(&child->sibling, zombies);
    }
}

/* tasklist_lock held. one thread of the process, the one least in the way of sig */
static int group_signal(struct task *t, int sig, const struct ksiginfo *info) {
    struct tgroup *g = t->group;
    if (!g) return send_signal(t, sig, info);

    struct task *target = NULL;

    for (list_node *n = g->threads.next; n != &g->threads; n = n->next) {
        struct task *th = container_of(n, struct task, group_node);

        if (!target) target = th; /* all of them block it: it waits on the first */
        if (!(__atomic_load_n(&th->blocked, 0) & sigmask(sig))) { /* __ATOMIC_RELAXED */
            target = th;
            break;
        }
    }

    return target ? send_signal(target, sig, info) : 0; /* no thread left: like a zombie, nothing to do */
}

/* finish_task_switch(), after the rq lock is dropped. when a process's last thread is off its cpu, the process is done */
void task_dead(struct task *task) {
    struct tgroup *g = task->group;
    struct task *leader = g ? g->leader : task;

    list_node zombies;
    list_init(&zombies);

    bool done = true, free_leader = false;

    uint64_t f = spin_lock_irqsave(&tasklist_lock);

    if (g) {
        list_del(&task->group_node);
        done = --g->live == 0;
    }

    if (done) {
        orphan_children(leader, &zombies);

        if (g && g->exiting) leader->exit_code = g->exit_code;

        if (leader->parent) {
            struct ksiginfo info = { .signo = SIGCHLD, .code = CLD_EXITED, .pid = leader->id };

            leader->state = TASK_ZOMBIE;
            wake_up(&leader->parent->wait_chldexit);
            group_signal(leader->parent, SIGCHLD, &info); /* a kthread parent takes none */
        } else {
            free_leader = true;
        }
    }

    spin_unlock_irqrestore(&tasklist_lock, f);

    /* nobody is left for its SIGALRM */
    if (done && g) timer_del(&g->itimer);

    while (!list_empty(&zombies)) {
        struct task *child = list_first(&zombies, struct task, sibling);

        list_del(&child->sibling);
        task_free(child);
    }

    /* a thread frees itself. a leader stays for its parent, or until its process is done */
    if (task != leader) task_free(task);
    if (free_leader) task_free(leader);
}

/* 0: not exited yet, -ECHILD: no such child */
/* pid: a child, -1 any, 0 one in the caller's group, below -1 one in group -pid */
static long reap_child(struct task *curr, long pid, long *status) {
    struct task *zombie = NULL;
    bool found = false;

    uint64_t f = spin_lock_irqsave(&tasklist_lock);

    long pgid = pid < -1 ? -pid : curr->group ? (long)curr->group->pgid : (long)curr->id;

    for (list_node *n = curr->children.next; n != &curr->children; n = n->next) {
        struct task *c = container_of(n, struct task, sibling);

        if (pid > 0 && (long)c->id != pid) continue;
        if (pid < 1 && pid != -1 && (c->group ? (long)c->group->pgid : (long)c->id) != pgid) continue;

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

/* pid -1: any child. the children are the process's, on its leader */
long task_waitpid(long pid, long *status, int options) {
    struct task *curr = current_task();
    struct task *leader = curr->group ? curr->group->leader : curr;
    long ret;

    if (options & WNOHANG) return reap_child(leader, pid, status);

    /* a signal ends the wait: -ERESTARTSYS, which the way back to user turns into a restart or -EINTR */
    int err = wait_event_interruptible(leader->wait_chldexit, (ret = reap_child(leader, pid, status)) != 0);
    return err ? err : ret;
}

/* child NULL: any child */
long task_wait(struct task *child, long *status, int options) {
    return task_waitpid(child ? (long)child->id : -1, status, options);
}

/* tasklist_lock held */
static struct task *pid_find(long id) {
    for (struct task *t = pid_hash[id % PID_HASH]; t; t = t->pid_next)
        if ((long)t->id == id) return t;

    return NULL;
}

/* the open files of a process, for /proc: referenced (files_put() them), NULL if there is none. pid leads a process */
struct files *task_files_get(uint64_t pid) {
    struct files *files = NULL;

    uint64_t f = spin_lock_irqsave(&tasklist_lock);

    struct task *t = pid_find(pid);

    if (t && t->files && t->group && t->group->tgid == pid) {
        files = t->files;
        files_get(files);
    }

    spin_unlock_irqrestore(&tasklist_lock, f);
    return files;
}

/* kill(): the process pid leads */
int task_signal(long pid, int sig, const struct ksiginfo *info) {
    int err = -ESRCH;

    uint64_t f = spin_lock_irqsave(&tasklist_lock);

    struct task *t = pid_find(pid);
    if (t && (!t->group || t->group->tgid == (uint64_t)pid)) err = sig ? group_signal(t, sig, info) : 0;

    spin_unlock_irqrestore(&tasklist_lock, f);
    return err;
}

/* tkill(), tgkill(): that thread */
int task_signal_thread(long tgid, long tid, int sig, const struct ksiginfo *info) {
    int err = -ESRCH;

    uint64_t f = spin_lock_irqsave(&tasklist_lock);

    struct task *t = pid_find(tid);
    if (t && (!tgid || (t->group && t->group->tgid == (uint64_t)tgid))) err = sig ? send_signal(t, sig, info) : 0;

    spin_unlock_irqrestore(&tasklist_lock, f);
    return err;
}

/* off the mm before the end: a zombie holds no address space. the last thread's put frees it */
static void exit_mm(struct task *curr) {
    struct mm *mm = curr->mm;
    if (!mm) return;

    interrupt_status_t flags = arch_irq_save();

    pmap_switch(curr->pmap, pmap_kernel());
    curr->mm = NULL;
    curr->pmap = pmap_kernel();
    pmap_activate(pmap_kernel());

    arch_irq_restore(flags);

    mm_put(mm);
    vfork_release(curr);
}

/* this thread only. the process ends with its last thread */
void task_exit(long code) {
    struct task *curr = current_task();

    futex_exit_robust(curr);

    /* pthread_join() sleeps on this word */
    if (curr->clear_child_tid) {
        uint32_t zero = 0;

        if (copy_to_user(curr->clear_child_tid, &zero, sizeof(zero)) == 0)
            futex_wake(curr->mm, curr->clear_child_tid, 1);

        curr->clear_child_tid = 0;
    }

    /* while we still run: task_free() may come from finish_task_switch(), irq off */
    if (curr->files) {
        files_put(curr->files);
        curr->files = NULL;
    }

    exit_mm(curr);

    arch_irq_disable();

    curr->exit_code = code;
    curr->state = TASK_DEAD;
    schedule();

    panic("sched: dead task got scheduled -- how?");
}

/* exit_group(), or a fatal signal: every thread goes, and code is the process's status */
void task_group_exit(long code) {
    struct task *curr = current_task();
    struct tgroup *g = curr->group;

    if (g) {
        uint64_t f = spin_lock_irqsave(&tasklist_lock);

        if (g->exiting) {
            code = g->exit_code;
        } else {
            struct ksiginfo info = { .signo = SIGKILL, .code = SI_KERNEL };

            g->exiting = true;
            g->exit_code = code;

            for (list_node *n = g->threads.next; n != &g->threads; n = n->next) {
                struct task *th = container_of(n, struct task, group_node);
                if (th != curr) send_signal(th, SIGKILL, &info);
            }
        }

        spin_unlock_irqrestore(&tasklist_lock, f);
    }

    task_exit(code);
}

/* the process's parent, which is the leader's. 1 once orphaned */
long sys_getppid(void) {
    struct task *leader = current_task()->group->leader;

    uint64_t f = spin_lock_irqsave(&tasklist_lock);
    long ppid = leader->parent ? (long)leader->parent->id : 1;
    spin_unlock_irqrestore(&tasklist_lock, f);

    return ppid;
}

/* tasklist_lock held. the leader of the process pid, if there is one */
static struct task *process_find(long pid) {
    struct task *t = pid_find(pid);

    return t && t->group && t->group->tgid == (uint64_t)pid ? t : NULL;
}

/* tasklist_lock held. is anyone in the group, of that session */
static bool pgrp_exists(uint64_t pgid, uint64_t sid) {
    for (int i = 0; i < PID_HASH; i++) {
        for (struct task *t = pid_hash[i]; t; t = t->pid_next)
            if (t->group && t->group->leader == t && t->group->pgid == pgid && t->group->sid == sid) return true;
    }

    return false;
}

int task_signal_pgrp(long pgid, int sig, const struct ksiginfo *info) {
    int err = -ESRCH;

    uint64_t f = spin_lock_irqsave(&tasklist_lock);

    for (int i = 0; i < PID_HASH; i++) {
        for (struct task *t = pid_hash[i]; t; t = t->pid_next) {
            if (!t->group || t->group->leader != t || t->group->pgid != (uint64_t)pgid) continue;

            err = 0;
            if (sig) group_signal(t, sig, info);
        }
    }

    spin_unlock_irqrestore(&tasklist_lock, f);
    return err;
}

long sys_getpgid(long pid) {
    struct task *curr = current_task();
    long ret = -ESRCH;

    uint64_t f = spin_lock_irqsave(&tasklist_lock);

    struct task *t = pid ? process_find(pid) : curr->group->leader;
    if (t) ret = t->group->pgid;

    spin_unlock_irqrestore(&tasklist_lock, f);
    return ret;
}

long sys_getpgrp(void) {
    return sys_getpgid(0);
}

long sys_getsid(long pid) {
    struct task *curr = current_task();
    long ret = -ESRCH;

    uint64_t f = spin_lock_irqsave(&tasklist_lock);

    struct task *t = pid ? process_find(pid) : curr->group->leader;
    if (t) ret = t->group->sid;

    spin_unlock_irqrestore(&tasklist_lock, f);
    return ret;
}

/* the caller or one of its children, into an existing group of its session or a new one of its own */
long sys_setpgid(long pid, long pgid) {
    struct task *leader = current_task()->group->leader;
    long ret = 0;

    if (pgid < 0) return -EINVAL;

    uint64_t f = spin_lock_irqsave(&tasklist_lock);

    struct task *t = pid ? process_find(pid) : leader;

    if (!t || (t != leader && t->parent != leader)) {
        ret = -ESRCH;
    } else if ((uint64_t)(pgid ? pgid : t->group->tgid) == t->group->pgid) {
        ret = 0; /* already there. the first process is a session leader, and its shell asks for this */
    } else if (t->group->sid != leader->group->sid || t->group->sid == t->group->tgid) {
        ret = -EPERM; /* another session's, or a session leader */
    } else {
        if (!pgid) pgid = t->group->tgid;

        if ((uint64_t)pgid != t->group->tgid && !pgrp_exists(pgid, t->group->sid)) ret = -EPERM;
        else t->group->pgid = pgid;
    }

    spin_unlock_irqrestore(&tasklist_lock, f);
    return ret;
}

/* a session of its own, and the group in it. not for a group's leader: its group has members to leave behind */
long sys_setsid(void) {
    struct tgroup *g = current_task()->group;
    long ret;

    uint64_t f = spin_lock_irqsave(&tasklist_lock);

    if (g->pgid == g->tgid) {
        ret = -EPERM;
    } else {
        g->sid = g->pgid = g->tgid;
        ret = g->tgid;
    }

    spin_unlock_irqrestore(&tasklist_lock, f);
    return ret;
}

/* setpriority()/getpriority() of one thread. 0: the caller */
long task_nice(long tid, int *nice, bool set) {
    long ret = -ESRCH;

    uint64_t f = spin_lock_irqsave(&tasklist_lock);

    struct task *t = tid ? pid_find(tid) : current_task();

    if (t && t->group) {
        if (set) sched_set_nice(t, *nice);
        else     *nice = t->nice;

        ret = 0;
    }

    spin_unlock_irqrestore(&tasklist_lock, f);
    return ret;
}
