OUTPUT := serika.elf
ARCH   ?= x86_64

export BASE_DIR   := $(shell pwd)
export BUILD_DIR  := $(BASE_DIR)/build
export DEPS_DIR   := $(BASE_DIR)/deps
export BIN_DIR    := $(BASE_DIR)/bin
export OBJ_DIR    := $(BUILD_DIR)/obj

SETUP_FLAG := .setup_done

IMAGE      := $(BIN_DIR)/serika.iso
IMAGE_ROOT := $(BUILD_DIR)/iso_root

LIMINE_DIR := $(DEPS_DIR)/limine

SUBDIRS := init

ifeq ($(ARCH), x86_64)
    ARCH_CFLAGS  := -m64 -march=x86-64 -mno-red-zone
    ARCH_LDFLAGS := -m elf_x86_64
    LIMINE_EFI   := $(LIMINE_DIR)/BOOTX64.EFI
	# add your architecture here...
endif

export CC := clang -target $(ARCH)-unknown-none-elf
export LD := ld.lld

export CFLAGS   := -g -O2 -ffreestanding -fno-stack-protector \
				   -fno-PIC -fno-PIE \
                   $(ARCH_CFLAGS) \
                   -ffunction-sections -fdata-sections -Wall -Wextra

export CPPFLAGS := -I$(BASE_DIR) \
				   -I$(BASE_DIR)/include \
                   -D__$(ARCH)__

export LDFLAGS  := $(ARCH_LDFLAGS) -nostdlib -static --gc-sections \
				   -no-pie -z max-page-size=0x1000 -T $(BASE_DIR)/arch/$(ARCH)/kernel/linker.ld

.PHONY: all clean $(SUBDIRS) run build setup

all: setup_check $(BIN_DIR)/$(OUTPUT)

setup_check:
ifeq ($(wildcard $(SETUP_FLAG)),)
	@echo =========================
	@echo run \'make setup\' first!
	@echo =========================
	@exit 1
endif

$(SUBDIRS):
	@mkdir -p $(OBJ_DIR)/$@
	@$(MAKE) -C $@ ARCH=$(ARCH)

$(BIN_DIR)/$(OUTPUT): $(SUBDIRS)
	@mkdir -p $(BIN_DIR)
	@echo "LD: $@"
	$(eval ALL_OBJS := $(shell find $(OBJ_DIR) -name "*.o"))
	$(LD) $(LDFLAGS) $(ALL_OBJS) -o $@

setup:
	@if [ ! -d "$(LIMINE_DIR)" ]; then \
		git clone https://github.com/limine-bootloader/limine $(LIMINE_DIR) --branch=v11.x-binary; \
	fi
	touch $(SETUP_FLAG)

build: all
	@rm -rf   $(IMAGE_ROOT)

	@mkdir -p $(IMAGE_ROOT)/boot/limine
	@mkdir -p $(IMAGE_ROOT)/EFI/BOOT

	# copy limine files
	@cp -v $(LIMINE_DIR)/limine-bios.sys \
	       $(LIMINE_DIR)/limine-bios-cd.bin \
	       $(LIMINE_DIR)/limine-uefi-cd.bin \
	       $(IMAGE_ROOT)/boot/limine/

	# limine config
	@cp -v $(BASE_DIR)/limine.conf $(IMAGE_ROOT)/boot/limine/

	# kernel
	@cp -v $(BIN_DIR)/$(OUTPUT) $(IMAGE_ROOT)/boot/

	# limine EFIs
	@cp -v $(LIMINE_EFI) $(IMAGE_ROOT)/EFI/BOOT/

	# build iso
	@xorriso -as mkisofs \
		-R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot \
		-boot-load-size 4 \
		-boot-info-table \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part \
		--efi-boot-image \
		$(IMAGE_ROOT) -o $(IMAGE)

run: build
	qemu-system-x86_64 \
		-machine q35 \
		-cdrom $(IMAGE) \
		-m 8G \
		-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
		-bios /usr/share/OVMF/OVMF_CODE.fd \
		-serial stdio -smp 4 -accel kvm -cpu host \

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

distclean: clean
	rm -rf $(DEPS_DIR) $(SETUP_FLAG)