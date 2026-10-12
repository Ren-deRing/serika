INITRD_DIR := $(BUILD_DIR)/initrd
INITRD     := $(BUILD_DIR)/initrd.cpio

# against the host's shared glibc: ld.so and the libraries go in /lib64
GLIBC_CC   := clang
GLIBC_BINS := hello files signals fork threads syscalls gltest fbdemo sockets events

# what a program needs beyond libc and libm
LIBS_gltest := -lEGL -lGLESv2
LIBS_fbdemo := -lEGL -lGLESv2
GLIBC_LIBS := ld-linux-x86-64.so.2 libc.so.6 libm.so.6 libgcc_s.so.1
EXEC       := $(GLIBC_BINS:%=$(INITRD_DIR)/bin/%) $(GLIBC_LIBS:%=$(INITRD_DIR)/lib64/%)

# the host's busybox, static, and a link to it for each applet
APPLETS := $(BUILD_DIR)/applets.stamp
EXEC    += $(INITRD_DIR)/bin/busybox $(APPLETS)

# the host's mesa with llvmpipe, and what it loads: llvm is most of it
MESA := $(BUILD_DIR)/mesa.stamp
EXEC += $(MESA)

# the host's clang, to compile and link on serika itself
TOOLCHAIN := $(BUILD_DIR)/toolchain.stamp
EXEC      += $(TOOLCHAIN)

.PHONY: all $(INITRD)

all: $(INITRD)

$(GLIBC_BINS:%=$(INITRD_DIR)/bin/%): $(INITRD_DIR)/bin/%: glibc/%.c
	@mkdir -p $(dir $@)
	@echo "GLIBC : $@"
	@$(GLIBC_CC) -O2 -Wall -Wextra $< -o $@ -lm $(LIBS_$*)

$(INITRD_DIR)/bin/busybox: /usr/bin/busybox
	@mkdir -p $(dir $@)
	@echo "CP    : $@"
	@cp -L $< $@

$(APPLETS): $(INITRD_DIR)/bin/busybox
	@echo "LN    : busybox applets"
	@for a in $$($< --list); do ln -sf busybox $(INITRD_DIR)/bin/$$a; done
	@touch $@

$(MESA): mesa.sh /usr/lib64/libEGL.so.1
	@echo "MESA  : llvmpipe"
	@./mesa.sh $(INITRD_DIR)
	@touch $@

$(TOOLCHAIN): toolchain.sh /usr/bin/clang
	@echo "TOOLCH: clang"
	@./toolchain.sh $(INITRD_DIR)
	@touch $@

$(INITRD_DIR)/lib64/%: /lib64/%
	@mkdir -p $(dir $@)
	@echo "CP    : $@"
	@cp -L $< $@

$(INITRD): $(EXEC)
	@cp -r root/. $(INITRD_DIR)/
	@echo "CPIO  : $@"
	@cd $(INITRD_DIR) && find . | LC_ALL=C sort | cpio -o -H newc --quiet > $@
