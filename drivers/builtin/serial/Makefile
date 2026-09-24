SERIAL_DIR := drivers/builtin/serial

SERIAL_SRCS_C := uart16550.c
SERIAL_SRCS_S :=

SERIAL_OBJS := $(patsubst %.c,$(OBJ_DIR)/$(SERIAL_DIR)/%.c.o,$(SERIAL_SRCS_C))
SERIAL_OBJS += $(patsubst %.S,$(OBJ_DIR)/$(SERIAL_DIR)/%.S.o,$(SERIAL_SRCS_S))

DRV_BUILTIN += $(SERIAL_OBJS)

$(OBJ_DIR)/$(SERIAL_DIR)/%.c.o: $(SERIAL_DIR)/%.c
	@mkdir -p $(dir $@)
	@echo "CC: $<"
	@$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@
