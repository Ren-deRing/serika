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

## Implementation
The tracing system implementation is organized into the following files:

- kernel/ftrace.c:                   Core ftrace subsystem and runtime tracing logic
- include/ftrace.h:                  Public kernel interface for ftrace
- arch/(ARCH)/kernel/ftrace.c:       arch-specific ftrace implementation
- arch/(ARCH)/include/asm/ftrace.h:	 arch ftrace definitions and architecture-specific interfaces

- arch/(ARCH)/kernel/patch.c:	     Runtime instruction patching
- arch/(ARCH)/include/asm/patch.h:	 Architecture-specific instruction patching interface

Architecture-specific implementations must provide the interfaces required by the generic ftrace subsystem.
The compiler-generated instrumentation points are handled by the architecture-specific ftrace implementation.
Runtime patching replaces the default instructions with calls to the tracing handler.
