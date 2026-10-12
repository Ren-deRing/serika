#pragma once

#include <asm/cpu.h>
#include <asm/task.h>
#include <asm/trapframe.h>

#include <serika/list.h>
#include <serika/signal.h>
#include <serika/wait.h>

#include <deps/tree.h>
#include <stdint.h>

#define TASK_KSTACK_SIZE (1024 * 16)

#define USER_STACK_TOP  0x7ffffffff000ULL
#define USER_STACK_SIZE (1024 * 1024)
#define USER_CODE_BASE  0x100000ULL

typedef void (*task_entry_t)(void);

enum task_state {
    TASK_RUNNING,
    TASK_RUNNABLE,
    TASK_SLEEPING,
    TASK_DEAD,
    TASK_ZOMBIE,
};

struct task;
struct vfork_done;

struct tgroup {
    uint32_t     users;     /* tasks pointing at it, atomic */
    uint64_t     tgid;      /* the leader's id, getpid() */
    uint64_t     pgid;      /* process group and session: a process id. tasklist_lock */
    uint64_t     sid;
    uint32_t     live;      /* threads that have not exited */
    list_node    threads;   /* the live ones, by group_node */
    struct task *leader;    /* reaped last: a zombie until live is 0 */
    bool         exiting;   /* exit_group() or a fatal signal started */
    long         exit_code; /* then everyone's, and the process's status */
};

struct task {
    uint64_t vruntime;
    uint64_t id;

    RB_ENTRY(task) sched_entry;

    void             *kstack;
    size_t            kernel_stack_size;

    task_entry_t      entry;
    
    /* arch_task_init */
    struct trapframe *tf;
    struct arch_task  arch;
    /* ============== */

    struct pmap      *pmap;          /* mm->pmap, or the kernel's */
    struct mm        *mm;            /* NULL for kthreads */
    struct files     *files;         /* NULL for kthreads */
    struct sighand   *sighand;       /* NULL for kthreads */

    struct task      *parent;        /* NULL: frees itself on exit. tasklist_lock */
    list_node         children;      /* tasklist_lock */
    list_node         sibling;       /* in parent->children */
    struct wait_queue wait_chldexit; /* children exiting */

    enum task_state state;

    int               nice;
    uint32_t          weight;

    uint64_t          exec_start;    /* tsc */
    uint64_t          sum_exec;      /* tsc */
    uint64_t          prev_sum_exec; /* sum_exec when picked */

    bool              pinned;        /* never migrates to another cpu */

    uint32_t          cpu;           /* changes under rq's lock */
    uint32_t          saved_preempt_count; /* the cpu's count while we are switched out */
    bool              on_rq;         /* curr or in the tree. rq lock */
    long              exit_code;     /* wait status, linux encoding */

    uint64_t          saved_blocked; /* ppoll() and pselect(): the mask to go back to */
    bool              restore_sigmask;
    uint64_t          blocked;       /* signals. only the task changes it, under sighand lock */
    uint64_t          pending;       /* sighand lock, atomic to read */
    struct ksiginfo   siginfo[NSIG]; /* of each pending signal */

    struct task      *pid_next;      /* pid hash. tasklist_lock */

    struct tgroup    *group;         /* NULL for kthreads */
    list_node         group_node;    /* in group->threads while alive */
    char              comm[16];      /* prctl(PR_SET_NAME), for whoever asks */
    struct vfork_done *vfork_done; /* the parent waits for this task to exec or exit */
    uintptr_t         robust_list;   /* user's robust_list_head: locks it held are marked on exit */
    uintptr_t         clear_child_tid; /* zeroed and futex-woken on exit: pthread_join waits on it */
};

void switch_to(struct task *prev, struct task *next);

void task_start(void);
void task_start_user(void);

struct task *task_create(task_entry_t entry, int nice);
struct task *task_create_mm(struct mm *mm, struct files *files, struct sighand *sighand,
                            uintptr_t entry, uintptr_t user_stack, int nice);
struct task *task_create_user(const void *code, size_t size, int nice);
struct task *task_create_thread(struct task *proc, uintptr_t entry, uintptr_t user_stack, int nice);
struct task *task_create_boot(void);
void task_free(struct task *task);
void task_dead(struct task *task);
void task_exit(long code);
long task_wait(struct task *child, long *status, int options);
long task_waitpid(long pid, long *status, int options);
int task_fork(const struct trapframe *tf, bool share_mm, bool clear_sighand, struct task **out);

/* clone(CLONE_VFORK): the parent sleeps until the child has left the mm it shares, by exec or exit */
struct vfork_done *vfork_alloc(void);
void vfork_attach(struct task *child, struct vfork_done *vd);
void vfork_wait(struct vfork_done *vd);
void vfork_cancel(struct vfork_done *vd);
int task_clone_thread(const struct trapframe *tf, struct task **out);
void task_group_exit(long code);
int task_signal_thread(long tgid, long tid, int sig, const struct ksiginfo *info); /* tgid 0: any */
void task_exec_mm(struct mm *mm);
long task_nice(long tid, int *nice, bool set); /* one thread. 0: the caller */
int task_signal_pgrp(long pgid, int sig, const struct ksiginfo *info); /* every process in it */
int task_signal(long pid, int sig, const struct ksiginfo *info); /* the process. sig 0: is it there */
