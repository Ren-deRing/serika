#pragma once

#include <serika/list.h>

#include <stdbool.h>
#include <stdint.h>

/* fires once, from cpu 0's tick, with timer_lock held: fn must not sleep or take timer_lock */
struct timer {
    list_node node;
    uint64_t  expires; /* tsc */
    void    (*fn)(struct timer *t);
    bool      queued;  /* timer_lock */
};

void timer_add(struct timer *t);
bool timer_del(struct timer *t); /* true if it had not fired. a running fn is over once this returns */
void timer_tick(void);           /* cpu 0's tick */

long sleep_until(uint64_t deadline); /* tsc. a signal ends it early: -EINTR */

/* a user struct timespec as a tsc count */
int timespec_to_tsc(long uts, uint64_t *tsc);
