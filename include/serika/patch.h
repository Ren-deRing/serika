#include <serika/compiler.h>

#include <stddef.h>

notrace void text_patch(void *addr, const void *opcode, size_t len);
