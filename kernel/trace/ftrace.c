#include <serika/compiler.h>
#include <serika/ftrace.h>
#include <serika/printk.h>
#include <asm/patch.h>

#include <stdint.h>

notrace void ftrace_trace(uintptr_t ip)
{
    printk("ftrace: %p\n", (void *)ip);
}