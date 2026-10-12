#pragma once

#include <serika/task.h>

/* argv and envp end with NULL, either may be NULL. *out is not runnable until sched_add_task() */
int task_create_exec(const char *path, const char *const argv[], const char *const envp[], int nice,
                     struct task **out);

struct mm;

/* a new address space with path loaded and its stack built. name is how the caller spelled it: a script gets
 * that, and AT_EXECFN */
int exec_load(const char *path, const char *name, const char *const argv[], const char *const envp[],
              struct mm **out, uintptr_t *entry, uintptr_t *sp);
