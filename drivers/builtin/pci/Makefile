PCI_DIR := drivers/builtin/pci

PCI_SRCS_C := pci.c
PCI_SRCS_S :=

PCI_OBJS := $(patsubst %.c,$(OBJ_DIR)/$(PCI_DIR)/%.c.o,$(PCI_SRCS_C))
PCI_OBJS += $(patsubst %.S,$(OBJ_DIR)/$(PCI_DIR)/%.S.o,$(PCI_SRCS_S))

DRV_BUILTIN += $(PCI_OBJS)

$(OBJ_DIR)/$(PCI_DIR)/%.c.o: $(PCI_DIR)/%.c
	@mkdir -p $(dir $@)
	@echo "CC    : $<"
	@$(CC) $(CFLAGS) $(DRV_CFLAGS) $(CPPFLAGS) -c $< -o $@

-include $(PCI_OBJS:.o=.d)
