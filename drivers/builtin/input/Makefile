INPUT_DIR := drivers/builtin/input

INPUT_SRCS_C := evdev.c
INPUT_SRCS_S :=

INPUT_OBJS := $(patsubst %.c,$(OBJ_DIR)/$(INPUT_DIR)/%.c.o,$(INPUT_SRCS_C))
INPUT_OBJS += $(patsubst %.S,$(OBJ_DIR)/$(INPUT_DIR)/%.S.o,$(INPUT_SRCS_S))

DRV_BUILTIN += $(INPUT_OBJS)

$(OBJ_DIR)/$(INPUT_DIR)/%.c.o: $(INPUT_DIR)/%.c
	@mkdir -p $(dir $@)
	@echo "CC    : $<"
	@$(CC) $(CFLAGS) $(DRV_CFLAGS) $(CPPFLAGS) -c $< -o $@

-include $(INPUT_OBJS:.o=.d)
