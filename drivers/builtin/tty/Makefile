TTY_DIR := drivers/builtin/tty

TTY_SRCS_C := tty_io.c n_tty.c pty.c
TTY_SRCS_S :=

TTY_OBJS := $(patsubst %.c,$(OBJ_DIR)/$(TTY_DIR)/%.c.o,$(TTY_SRCS_C))
TTY_OBJS += $(patsubst %.S,$(OBJ_DIR)/$(TTY_DIR)/%.S.o,$(TTY_SRCS_S))

DRV_BUILTIN += $(TTY_OBJS)

$(OBJ_DIR)/$(TTY_DIR)/%.c.o: $(TTY_DIR)/%.c
	@mkdir -p $(dir $@)
	@echo "CC    : $<"
	@$(CC) $(CFLAGS) $(DRV_CFLAGS) $(CPPFLAGS) -c $< -o $@

-include $(TTY_OBJS:.o=.d)
