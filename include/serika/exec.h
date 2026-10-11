#pragma once

#include <serika/task.h>

/* argv and envp end with NULL, either may be NULL. *out is not runnable until sched_add_task() */
int task_create_exec(const char *path, const char *const argv[], const char *const envp[], int nice,
                     struct task **out);
