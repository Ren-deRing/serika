SRCS_C := $(shell find . -name "*.c")
SRCS_S := $(shell find . -name "*.S")

OBJS := $(patsubst ./%.c,$(OBJ_DIR)/kernel/%.c.o,$(SRCS_C))
OBJS += $(patsubst ./%.S,$(OBJ_DIR)/kernel/%.S.o,$(SRCS_S))

CFLAGS := $(CFLAGS) -mcmodel=kernel -mno-sse -mno-sse2 -mno-mmx -mno-avx

all: $(OBJS)

$(OBJ_DIR)/kernel/%.c.o: %.c
	@mkdir -p $(dir $@)
	@echo "CC: $<"
	@$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(OBJ_DIR)/kernel/%.S.o: %.S
	@mkdir -p $(dir $@)
	@echo "CC: $<"
	@$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@
