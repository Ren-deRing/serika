INITRD_DIR := $(BUILD_DIR)/initrd
INITRD     := $(BUILD_DIR)/initrd.cpio
# not under OBJ_DIR: the kernel links every .o there
UOBJ_DIR   := $(BUILD_DIR)/user

UCC     := clang -target x86_64-unknown-none-elf
UCFLAGS := -g -O2 -ffreestanding -fno-stack-protector -Wall -Wextra -Ilib -MMD -MP

LIB  := lib/start.S lib/string.c
BINS := check

# each bin twice: static at a fixed address, and static-pie
EXEC := $(BINS:%=$(INITRD_DIR)/bin/%) $(BINS:%=$(INITRD_DIR)/bin/%-pie)

# static glibc from the host (glibc-static)
GLIBC_CC   := clang
GLIBC_BINS := hello files signals
EXEC       += $(GLIBC_BINS:%=$(INITRD_DIR)/bin/%)

LIB_OBJS     := $(LIB:%=$(UOBJ_DIR)/%.o)
LIB_PIE_OBJS := $(LIB:%=$(UOBJ_DIR)/pie/%.o)

.PHONY: all $(INITRD)
.SECONDARY: # keeps bin objects, they are intermediates

all: $(INITRD)

$(UOBJ_DIR)/%.o: %
	@mkdir -p $(dir $@)
	@echo "UCC   : $<"
	@$(UCC) $(UCFLAGS) -fno-pic -c $< -o $@

$(UOBJ_DIR)/pie/%.o: %
	@mkdir -p $(dir $@)
	@echo "UCC   : $< (pie)"
	@$(UCC) $(UCFLAGS) -fPIE -c $< -o $@

$(GLIBC_BINS:%=$(INITRD_DIR)/bin/%): $(INITRD_DIR)/bin/%: glibc/%.c
	@mkdir -p $(dir $@)
	@echo "GLIBC : $@"
	@$(GLIBC_CC) -static -O2 -Wall -Wextra $< -o $@ -lm

$(INITRD_DIR)/bin/%-pie: $(UOBJ_DIR)/pie/bin/%.c.o $(LIB_PIE_OBJS)
	@mkdir -p $(dir $@)
	@echo "ULD   : $@"
	@ld.lld -pie --no-dynamic-linker -z text -e _start $^ -o $@

$(INITRD_DIR)/bin/%: $(UOBJ_DIR)/bin/%.c.o $(LIB_OBJS)
	@mkdir -p $(dir $@)
	@echo "ULD   : $@"
	@ld.lld -static -e _start $^ -o $@

$(INITRD): $(EXEC)
	@cp -r root/. $(INITRD_DIR)/
	@echo "CPIO  : $@"
	@cd $(INITRD_DIR) && find . | LC_ALL=C sort | cpio -o -H newc --quiet > $@

-include $(shell find $(UOBJ_DIR) -name "*.d" 2>/dev/null)
