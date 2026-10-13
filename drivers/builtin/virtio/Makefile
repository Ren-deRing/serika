VIRTIO_DIR := drivers/builtin/virtio

VIRTIO_SRCS_C := virtio.c virtio_input.c
VIRTIO_SRCS_S :=

VIRTIO_OBJS := $(patsubst %.c,$(OBJ_DIR)/$(VIRTIO_DIR)/%.c.o,$(VIRTIO_SRCS_C))
VIRTIO_OBJS += $(patsubst %.S,$(OBJ_DIR)/$(VIRTIO_DIR)/%.S.o,$(VIRTIO_SRCS_S))

DRV_BUILTIN += $(VIRTIO_OBJS)

$(OBJ_DIR)/$(VIRTIO_DIR)/%.c.o: $(VIRTIO_DIR)/%.c
	@mkdir -p $(dir $@)
	@echo "CC    : $<"
	@$(CC) $(CFLAGS) $(DRV_CFLAGS) $(CPPFLAGS) -c $< -o $@

-include $(VIRTIO_OBJS:.o=.d)
