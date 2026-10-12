INITRD_DIR := $(BUILD_DIR)/initrd
INITRD     := $(BUILD_DIR)/initrd.cpio

# against the host's shared glibc: ld.so and the libraries go in /lib64
GLIBC_CC   := clang
GLIBC_BINS := hello files signals fork threads syscalls
GLIBC_LIBS := ld-linux-x86-64.so.2 libc.so.6 libm.so.6 libgcc_s.so.1
EXEC       := $(GLIBC_BINS:%=$(INITRD_DIR)/bin/%) $(GLIBC_LIBS:%=$(INITRD_DIR)/lib64/%)

# the host's busybox, static
EXEC += $(INITRD_DIR)/bin/busybox

.PHONY: all $(INITRD)

all: $(INITRD)

$(GLIBC_BINS:%=$(INITRD_DIR)/bin/%): $(INITRD_DIR)/bin/%: glibc/%.c
	@mkdir -p $(dir $@)
	@echo "GLIBC : $@"
	@$(GLIBC_CC) -O2 -Wall -Wextra $< -o $@ -lm

$(INITRD_DIR)/bin/busybox: /usr/bin/busybox
	@mkdir -p $(dir $@)
	@echo "CP    : $@"
	@cp -L $< $@

$(INITRD_DIR)/lib64/%: /lib64/%
	@mkdir -p $(dir $@)
	@echo "CP    : $@"
	@cp -L $< $@

$(INITRD): $(EXEC)
	@cp -r root/. $(INITRD_DIR)/
	@echo "CPIO  : $@"
	@cd $(INITRD_DIR) && find . | LC_ALL=C sort | cpio -o -H newc --quiet > $@
