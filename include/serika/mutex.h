#pragma once

#include <serika/wait.h>

#include <stdbool.h>

/* sleeping lock. task context only, never under a spinlock */
struct mutex {
    struct task      *owner; /* NULL: free */
    struct wait_queue wq;
};

void mutex_init(struct mutex *m);
void mutex_lock(struct mutex *m);
bool mutex_trylock(struct mutex *m);
void mutex_unlock(struct mutex *m);
