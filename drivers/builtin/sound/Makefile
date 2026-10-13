SOUND_DIR := drivers/builtin/sound

SOUND_SRCS_C := hda.c
SOUND_SRCS_S :=

SOUND_OBJS := $(patsubst %.c,$(OBJ_DIR)/$(SOUND_DIR)/%.c.o,$(SOUND_SRCS_C))
SOUND_OBJS += $(patsubst %.S,$(OBJ_DIR)/$(SOUND_DIR)/%.S.o,$(SOUND_SRCS_S))

DRV_BUILTIN += $(SOUND_OBJS)

$(OBJ_DIR)/$(SOUND_DIR)/%.c.o: $(SOUND_DIR)/%.c
	@mkdir -p $(dir $@)
	@echo "CC    : $<"
	@$(CC) $(CFLAGS) $(DRV_CFLAGS) $(CPPFLAGS) -c $< -o $@

-include $(SOUND_OBJS:.o=.d)
