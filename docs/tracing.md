# Kernel Tracing

## Function Tracing

All general kernel functions, after ftrace initialization, excluding 'notrace' functions, are a target for kernel tracing.

### notrace

The following functions do not allow instrumentation:

- Tracing subsystem itself
- Interrupt/exception entry
- Low-level assembly paths for context switches
- Functions where instrumentation could break the ABI or execution environment

`notrace` is explicitly specified by the developer.

## Instrumentation

Function tracing utilizes compiler instrumentation.

The compiler generates instrumentation points at function entry points during compilation.
The kernel patches these points at runtime to enable tracing. (yes, like linux)

By default, instrumentation points are nop instructions.
