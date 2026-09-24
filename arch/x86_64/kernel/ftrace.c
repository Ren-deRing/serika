#include <serika/compiler.h>
#include <serika/ftrace.h>
#include <serika/printk.h>
#include <serika/string.h>

#include <asm/patch.h>

#include <stdint.h>

extern void ftrace_caller();

extern uintptr_t __start_patchable_functions[];
extern uintptr_t __end_patchable_functions[];

notrace void patch_ftrace_call(void *target_func, void *tracer_func) {
    uint8_t buffer[5];
    
    buffer[0] = 0xE8;
    
    int32_t offset = (int32_t)((uintptr_t)tracer_func - ((uintptr_t)target_func + 5));
    memcpy(&buffer[1], &offset, sizeof(int32_t));

    text_patch(target_func, buffer, 5);
}

notrace void init_ftrace(void) {
    uintptr_t *addr = __start_patchable_functions;
    uintptr_t *end = __end_patchable_functions;
    
    int count = end - addr;
    
    printk("ftrace: %d functions to patch\n", count);
    printk("ftrace: tracer address: %lx\n", (uintptr_t)ftrace_caller);
    for (int i = 0; i<count; i++) {
        printk("function [%d]: %lx\n", i, addr[i]);
        patch_ftrace_call((void *) addr[i], ftrace_caller);
    }
}