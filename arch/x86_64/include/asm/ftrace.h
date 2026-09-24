#pragma once

#include <serika/compiler.h>
#include <serika/ftrace.h>

notrace void patch_ftrace(void *target_func, void *tracer_func, enum trace_t type);
