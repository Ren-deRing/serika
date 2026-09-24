#include <serika/compiler.h>
#include <serika/ftrace.h>
#include <serika/printk.h>

#include <asm/ftrace.h>
#include <asm/patch.h>

#include <stdint.h>

extern void ftrace_caller();

extern uintptr_t __start_patchable_functions[];
extern uintptr_t __end_patchable_functions[];

notrace void ftrace_trace(uintptr_t ip)
{
    printk("ftrace: %p\n", (void *)ip);
}

notrace void ftrace_enable(enum trace_t type) {
    uintptr_t *addr = __start_patchable_functions;
    uintptr_t *end = __end_patchable_functions;
    
    int count = end - addr;

    /* Keep these in sync with enum in include/serika/ftrace.h */
    const char* names[] = {
        [TRACE_CALL] = "TRACE_FUNC",
        [TRACE_NOP]  = "TRACE_NOP",
    };
    
    printk("ftrace: %d functions to patch\n", count);
    printk("ftrace: tracer type: %s\n", names[type]);

    printk("ftrace: tracer address: %lx\n", (uintptr_t)ftrace_caller);
    for (int i = 0; i<count; i++) {
        printk("function [%d]: %lx\n", i, addr[i]);
        patch_ftrace((void *) addr[i], ftrace_caller, type);
    }
}
