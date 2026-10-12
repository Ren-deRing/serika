DRM_DIR := drivers/builtin/drm

DRM_SRCS_C := drm.c gem.c kms.c
DRM_SRCS_S :=

DRM_OBJS := $(patsubst %.c,$(OBJ_DIR)/$(DRM_DIR)/%.c.o,$(DRM_SRCS_C))
DRM_OBJS += $(patsubst %.S,$(OBJ_DIR)/$(DRM_DIR)/%.S.o,$(DRM_SRCS_S))

DRV_BUILTIN += $(DRM_OBJS)

$(OBJ_DIR)/$(DRM_DIR)/%.c.o: $(DRM_DIR)/%.c
	@mkdir -p $(dir $@)
	@echo "CC    : $<"
	@$(CC) $(CFLAGS) $(DRV_CFLAGS) $(CPPFLAGS) -c $< -o $@

-include $(DRM_OBJS:.o=.d)
