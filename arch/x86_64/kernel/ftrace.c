#include <serika/compiler.h>
#include <serika/ftrace.h>
#include <serika/printk.h>
#include <serika/string.h>

#include <asm/patch.h>

#include <stdint.h>

typedef void (*builder_t)(uint8_t *, void *, void *);

notrace void build_inst_call(uint8_t *buffer, void *target_func, void *tracer_func) {
    /* 0xE8: CALL(rel32) */
    buffer[0] = 0xE8;

    int32_t offset = (int32_t)((uintptr_t)tracer_func - ((uintptr_t)target_func + 5));
    memcpy(&buffer[1], &offset, sizeof(int32_t));
}

notrace void build_inst_nop(uint8_t *buffer, void *target_func, void *tracer_func) {
    (void)target_func, (void)tracer_func;

    /* 5-byte NOP */
    buffer[0] = 0x0F; buffer[1] = 0x1F; buffer[2] = 0x44;
    buffer[3] = 0x00; buffer[4] = 0x00;
}

static builder_t build_table[] = {
    [TRACE_CALL] = (builder_t)build_inst_call,
    [TRACE_NOP]  = (builder_t)build_inst_nop,
};

notrace void patch_ftrace(void *target_func, void *tracer_func, enum trace_t type) {
    uint8_t buffer[5];

    build_table[type](buffer, target_func, tracer_func);
    text_patch(target_func, buffer, 5);
}
