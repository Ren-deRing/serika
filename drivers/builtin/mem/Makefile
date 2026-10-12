MEM_DIR := drivers/builtin/mem

MEM_SRCS_C := mem.c
MEM_SRCS_S :=

MEM_OBJS := $(patsubst %.c,$(OBJ_DIR)/$(MEM_DIR)/%.c.o,$(MEM_SRCS_C))
MEM_OBJS += $(patsubst %.S,$(OBJ_DIR)/$(MEM_DIR)/%.S.o,$(MEM_SRCS_S))

DRV_BUILTIN += $(MEM_OBJS)

$(OBJ_DIR)/$(MEM_DIR)/%.c.o: $(MEM_DIR)/%.c
	@mkdir -p $(dir $@)
	@echo "CC    : $<"
	@$(CC) $(CFLAGS) $(DRV_CFLAGS) $(CPPFLAGS) -c $< -o $@

-include $(MEM_OBJS:.o=.d)
