#pragma once

#include <serika/compiler.h>
#include <stdint.h>

enum trace_t { TRACE_CALL, TRACE_NOP };

notrace void ftrace_trace(uintptr_t ip);
notrace void ftrace_enable();
notrace void ftrace_disable();
notrace void patch_ftrace(void *target_func, void *tracer_func, enum trace_t type);
