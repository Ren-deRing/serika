FB_DIR := drivers/builtin/fb

FB_SRCS_C := fb.c
FB_SRCS_S :=

FB_OBJS := $(patsubst %.c,$(OBJ_DIR)/$(FB_DIR)/%.c.o,$(FB_SRCS_C))
FB_OBJS += $(patsubst %.S,$(OBJ_DIR)/$(FB_DIR)/%.S.o,$(FB_SRCS_S))

DRV_BUILTIN += $(FB_OBJS)

$(OBJ_DIR)/$(FB_DIR)/%.c.o: $(FB_DIR)/%.c
	@mkdir -p $(dir $@)
	@echo "CC    : $<"
	@$(CC) $(CFLAGS) $(DRV_CFLAGS) $(CPPFLAGS) -c $< -o $@

-include $(FB_OBJS:.o=.d)
