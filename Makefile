OUTPUT := serika.elf
ARCH   ?= x86_64

MAKEFLAGS += -s

export BASE_DIR   := $(shell pwd)
export BUILD_DIR  := $(BASE_DIR)/build
export DEPS_DIR   := $(BASE_DIR)/deps
export BIN_DIR    := $(BASE_DIR)/bin
export OBJ_DIR    := $(BUILD_DIR)/obj
export TOOL_DIR   := $(BASE_DIR)/tools

SETUP_FLAG := .setup_done

IMAGE      := $(BIN_DIR)/serika.iso
IMAGE_ROOT := $(BUILD_DIR)/iso_root

LIMINE_DIR := $(DEPS_DIR)/limine

KSYMBOLS_TOOL := $(TOOL_DIR)/ksymbols.py
KSYMBOLS_SRC  := $(BUILD_DIR)/ksymbols.c
KSYMBOLS_OBJ  := $(OBJ_DIR)/ksymbols.o

KOFFSETS_TOOL   := $(TOOL_DIR)/koffsets.py
KOFFSETS_HEADER := $(BASE_DIR)/include/serika/offsets.h

OFFSETS += include/serika/cpu.h cpu include/serika/task.h task include/serika/pmap.h pmap

# Except for drivers.
SUBDIRS := kernel init arch mm fs net tests

include drivers/Makefile

ifeq ($(ARCH), x86_64)
    ARCH_CFLAGS  := -m64 -march=x86-64 -mno-red-zone -fpatchable-function-entry=5
    ARCH_LDFLAGS := -m elf_x86_64
    LIMINE_EFI   := $(LIMINE_DIR)/BOOTX64.EFI
	OFFSETS      += arch/x86_64/include/asm/task.h arch_task arch/x86_64/include/asm/cpu.h arch_cpu 
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
				   -I$(BASE_DIR)/arch/$(ARCH)/include \
                   -D__$(ARCH)__ \
                   -MMD -MP

export LDFLAGS  := $(ARCH_LDFLAGS) -nostdlib -static --gc-sections \
				   -no-pie -z max-page-size=0x1000 -T $(BASE_DIR)/arch/$(ARCH)/kernel/linker.ld

.PHONY: all clean $(SUBDIRS) run build setup initrd

all: setup_check $(BIN_DIR)/$(OUTPUT)

setup_check:
ifeq ($(wildcard $(SETUP_FLAG)),)
	@echo =========================
	@echo run \'make setup\' first!
	@echo =========================
	@exit 1
endif

offsets:
	@echo "KOFFS : $(OFFSETS)"
	@python3 $(KOFFSETS_TOOL) $(OFFSETS) $(KOFFSETS_HEADER)

$(SUBDIRS):
	@mkdir -p $(OBJ_DIR)/$@
	@echo "MK    : $@"
	@$(MAKE) -C $@ ARCH=$(ARCH)

$(BIN_DIR)/$(OUTPUT): offsets $(SUBDIRS) $(DRV_BUILTIN)
	@# First pass

	@echo "KSYMS : $@ (stubs)"
	@python3 $(KSYMBOLS_TOOL) --stub $(KSYMBOLS_SRC)

	@echo "CC    : $(KSYMBOLS_SRC)"
	@mkdir -p $(dir $(KSYMBOLS_OBJ))
	@$(CC) $(CPPFLAGS) $(CFLAGS) -c $(KSYMBOLS_SRC) -o $(KSYMBOLS_OBJ)

	@mkdir -p $(BIN_DIR)
	@echo "LD    : $@"
	@$(LD) $(LDFLAGS) $$(find $(OBJ_DIR) -name "*.o" -not -path "$(OBJ_DIR)/drivers/*") $(DRV_BUILTIN) -o $@

	@echo "KSYMS : $@"
	@python3 $(KSYMBOLS_TOOL) $@ $(KSYMBOLS_SRC)

	@echo "CC    : $(KSYMBOLS_SRC)"
	@mkdir -p $(dir $(KSYMBOLS_OBJ))
	@$(CC) $(CPPFLAGS) $(CFLAGS) -c $(KSYMBOLS_SRC) -o $(KSYMBOLS_OBJ)

	@# Second pass

	@echo "LD    : $@ (ksymbols)"
	@$(LD) $(LDFLAGS) $$(find $(OBJ_DIR) -name "*.o" -not -path "$(OBJ_DIR)/drivers/*") $(DRV_BUILTIN) -o $@

	@echo "KSYMS : $@"
	@python3 $(KSYMBOLS_TOOL) $@ $(KSYMBOLS_SRC)

	@echo "CC    : $(KSYMBOLS_SRC)"
	@mkdir -p $(dir $(KSYMBOLS_OBJ))
	@$(CC) $(CPPFLAGS) $(CFLAGS) -c $(KSYMBOLS_SRC) -o $(KSYMBOLS_OBJ)

	@# Third pass

	@echo "LD    : $@ (ksymbols)"
	@$(LD) $(LDFLAGS) $$(find $(OBJ_DIR) -name "*.o" -not -path "$(OBJ_DIR)/drivers/*") $(DRV_BUILTIN) -o $@

setup:
	@if [ ! -d "$(LIMINE_DIR)" ]; then \
		git clone https://github.com/limine-bootloader/limine $(LIMINE_DIR) --branch=v11.x-binary; \
	fi
	touch $(SETUP_FLAG)

initrd:
	@echo "MK    : user"
	@$(MAKE) -C user

build: all initrd
	@rm -rf   $(IMAGE_ROOT)

	@mkdir -p $(IMAGE_ROOT)/boot/limine
	@mkdir -p $(IMAGE_ROOT)/EFI/BOOT

	@# copy limine files
	@echo "CP    : limine files"
	@cp $(LIMINE_DIR)/limine-bios.sys \
	       $(LIMINE_DIR)/limine-bios-cd.bin \
	       $(LIMINE_DIR)/limine-uefi-cd.bin \
	       $(IMAGE_ROOT)/boot/limine/

	@# limine config
	@echo "CP    : limine config"
	@cp $(BASE_DIR)/limine.conf $(IMAGE_ROOT)/boot/limine/

	@# kernel
	@echo "CP    : kernel"
	@cp $(BIN_DIR)/$(OUTPUT) $(IMAGE_ROOT)/boot/

	@# initrd
	@echo "CP    : initrd"
	@cp $(BUILD_DIR)/initrd.cpio $(IMAGE_ROOT)/boot/

	@# limine EFIs
	@echo "CP    : limine EFIs"
	@cp $(LIMINE_EFI) $(IMAGE_ROOT)/EFI/BOOT/

	@# build iso
	@echo "BUILD : ISO"
	@xorriso -as mkisofs \
		-quiet \
		-R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot \
		-boot-load-size 4 \
		-boot-info-table \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part \
		--efi-boot-image \
		$(IMAGE_ROOT) -o $(IMAGE) \

run: build
	@echo "QEMU  :"
	@qemu-system-x86_64 \
		-machine q35 \
		-cdrom $(IMAGE) \
		-m 4G \
		-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
		-bios /usr/share/OVMF/OVMF_CODE.fd \
		-chardev stdio,id=ser0,signal=off \
		-serial chardev:ser0 \
		-no-reboot \
		-smp 4 \
		-accel kvm -cpu host,tsc-freq=2500000000 \
#		-S -s

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

distclean: clean
	rm -rf $(DEPS_DIR) $(SETUP_FLAG)
