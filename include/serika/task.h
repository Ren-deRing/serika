#include <asm/cpu.h>
#include <asm/task.h>
#include <asm/trapframe.h>

#include <deps/tree.h>
#include <stdint.h>

#define TASK_KSTACK_SIZE (1024 * 16)

#define USER_STACK_TOP 0x7ffffffff000ULL
#define USER_CODE_BASE 0x100000ULL

typedef void (*task_entry_t)(void);

enum task_state {
    TASK_RUNNING,
    TASK_RUNNABLE,
    TASK_SLEEPING,
    TASK_DEAD,
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

    struct pmap      *pmap;

    enum task_state state;
};

void switch_to(struct task *prev, struct task *next);
