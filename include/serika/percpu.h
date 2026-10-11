#pragma once

/*
    this_cpu_read / this_cpu_write / this_cpu_add: a field of this cpu's struct cpu, one %gs
    instruction. curcpu->x is two loads, a tick can move the task in between.
    with irq on use these, curcpu-> is for irq off (schedule, interrupt handlers)
*/

#include <asm/percpu.h>
