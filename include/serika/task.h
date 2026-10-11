#pragma once

#include <asm/cpu.h>
#include <asm/task.h>
#include <asm/trapframe.h>

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

    struct task      *parent;

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
};

void switch_to(struct task *prev, struct task *next);

void task_start(void);
void task_start_user(void);

struct task *task_create(task_entry_t entry, int nice);
struct task *task_create_user(const void *code, size_t size, int nice);
struct task *task_create_boot(void);
void task_free(struct task *task);
void task_exit(long code);
long task_wait(struct task *child, long *status);
