#include <serika/compiler.h>
#include <serika/ftrace.h>
#include <serika/printk.h>

#include <serika/symbol.h>
#include <stdint.h>

extern void ftrace_caller();

extern uintptr_t __start_patchable_functions[];
extern uintptr_t __end_patchable_functions[];

notrace void ftrace_trace(uintptr_t callee, uintptr_t ret) {
    (void) ret;
    const struct ksymbol *symbol = ksymbol_find(callee);
    printk("[<%p>] %s\n", (void *)callee, ksymbol_name(symbol));
}

notrace static void ftrace_control(enum trace_t type) {
    uintptr_t *addr = __start_patchable_functions;
    uintptr_t *end = __end_patchable_functions;
    
    int count = end - addr;
    
    printk("ftrace: %d functions to patch\n", count);

    printk("ftrace: tracer address: %lx\n", (uintptr_t)ftrace_caller);
    for (int i = 0; i<count; i++) {
        printk("function [%d]: %lx\n", i, addr[i]);
        patch_ftrace((void *) addr[i], ftrace_caller, type);
    }
}

notrace void ftrace_enable() { ftrace_control(TRACE_CALL); }
notrace void ftrace_disable() { ftrace_control(TRACE_NOP); }
