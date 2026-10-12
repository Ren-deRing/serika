INITRD_DIR := $(BUILD_DIR)/initrd
INITRD     := $(BUILD_DIR)/initrd.cpio

# against the host's shared glibc: ld.so and the libraries go in /lib64
GLIBC_CC   := clang
GLIBC_BINS := hello files signals fork threads syscalls gltest fbdemo sockets events drmtest gbmtest pty

# what a program needs beyond libc and libm
LIBS_gltest := -lEGL -lGLESv2
LIBS_fbdemo := -lEGL -lGLESv2
LIBS_drmtest := -I/usr/include/libdrm -ldrm
LIBS_gbmtest := -I/usr/include/libdrm -lgbm -lEGL -lGLESv2 -ldrm
GLIBC_LIBS := ld-linux-x86-64.so.2 libc.so.6 libm.so.6 libgcc_s.so.1
EXEC       := $(GLIBC_BINS:%=$(INITRD_DIR)/bin/%) $(GLIBC_LIBS:%=$(INITRD_DIR)/lib64/%)

# the host's busybox, static, and a link to it for each applet
APPLETS := $(BUILD_DIR)/applets.stamp
EXEC    += $(INITRD_DIR)/bin/busybox $(APPLETS)

# the disk: an ext4 image that the kernel mounts on /usr. a tree is made as the host has it (/usr/...) and the
# image is what is under usr
DISK_ROOT := $(BUILD_DIR)/diskroot
DISK_DIR  := disk
DISK      := $(BUILD_DIR)/disk.img

# the host's mesa with llvmpipe, and what it loads: llvm is most of it
MESA := $(BUILD_DIR)/mesa.stamp

# the host's clang, to compile and link on serika itself
TOOLCHAIN := $(BUILD_DIR)/toolchain.stamp

# the compositor and what it loads
WAYLAND := $(BUILD_DIR)/wayland.stamp

.PHONY: all $(INITRD) $(DISK)

all: $(INITRD) $(DISK)

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
	@./mesa.sh $(DISK_ROOT)
	@touch $@

$(WAYLAND): wayland.sh /usr/bin/labwc
	@echo "WAYLND: labwc"
	@./wayland.sh $(DISK_ROOT)
	@touch $@

$(TOOLCHAIN): toolchain.sh /usr/bin/clang
	@echo "TOOLCH: clang"
	@./toolchain.sh $(DISK_ROOT)
	@touch $@

$(INITRD_DIR)/lib64/%: /lib64/%
	@mkdir -p $(dir $@)
	@echo "CP    : $@"
	@cp -L $< $@

$(INITRD): $(EXEC)
	@cp -r root/. $(INITRD_DIR)/
	@mkdir -p $(INITRD_DIR)/tmp $(INITRD_DIR)/usr $(INITRD_DIR)/sys/bus/platform $(INITRD_DIR)/proc
	@echo "CPIO  : $@"
	@cd $(INITRD_DIR) && find . | LC_ALL=C sort | cpio -o -H newc --quiet > $@

# no journal and no checksums: the kernel reads it without. room for what is in it and some for the tail of each block
$(DISK): $(MESA) $(TOOLCHAIN) $(WAYLAND) $(shell find $(DISK_DIR) -type f)
	@echo "EXT4  : $@"
	@mkdir -p $(DISK_ROOT)/usr
	@cp -r $(DISK_DIR)/. $(DISK_ROOT)/usr/
	@rm -f $@
	@mke2fs -q -t ext4 -b 4096 -i 8192 -L serika -m 0 -O ^has_journal,^metadata_csum,^orphan_file,^resize_inode \
		-E root_owner=0:0 -d $(DISK_ROOT)/usr $@ $$(( $$(du -sk $(DISK_ROOT)/usr | cut -f1) * 12 / 10 + 32768 ))K
