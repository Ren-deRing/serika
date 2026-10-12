BLOCK_DIR := drivers/builtin/block

BLOCK_SRCS_C := block.c
BLOCK_SRCS_S :=

BLOCK_OBJS := $(patsubst %.c,$(OBJ_DIR)/$(BLOCK_DIR)/%.c.o,$(BLOCK_SRCS_C))
BLOCK_OBJS += $(patsubst %.S,$(OBJ_DIR)/$(BLOCK_DIR)/%.S.o,$(BLOCK_SRCS_S))

DRV_BUILTIN += $(BLOCK_OBJS)

$(OBJ_DIR)/$(BLOCK_DIR)/%.c.o: $(BLOCK_DIR)/%.c
	@mkdir -p $(dir $@)
	@echo "CC    : $<"
	@$(CC) $(CFLAGS) $(DRV_CFLAGS) $(CPPFLAGS) -c $< -o $@

-include $(BLOCK_OBJS:.o=.d)
