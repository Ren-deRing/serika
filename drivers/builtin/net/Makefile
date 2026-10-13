NET_DIR := drivers/builtin/net

NET_SRCS_C := e1000e.c
NET_SRCS_S :=

NET_OBJS := $(patsubst %.c,$(OBJ_DIR)/$(NET_DIR)/%.c.o,$(NET_SRCS_C))
NET_OBJS += $(patsubst %.S,$(OBJ_DIR)/$(NET_DIR)/%.S.o,$(NET_SRCS_S))

DRV_BUILTIN += $(NET_OBJS)

$(OBJ_DIR)/$(NET_DIR)/%.c.o: $(NET_DIR)/%.c
	@mkdir -p $(dir $@)
	@echo "CC    : $<"
	@$(CC) $(CFLAGS) $(DRV_CFLAGS) $(CPPFLAGS) -c $< -o $@

-include $(NET_OBJS:.o=.d)
