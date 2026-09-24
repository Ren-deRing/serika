SRCS := $(shell find . \( -name "*.c" -o -name "*.S" \))
OBJS := $(patsubst ./%.c,$(OBJ_DIR)/init/%.c.o,$(SRCS))
OBJS += $(patsubst ./%.S,$(OBJ_DIR)/init/%.S.o,$(SRCS))

CFLAGS := $(CFLAGS) -mcmodel=kernel -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -mno-avx

all: $(OBJS)

$(OBJ_DIR)/init/%.c.o: %.c
	@mkdir -p $(dir $@)
	@echo "CC: $<"
	@$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(OBJ_DIR)/init/%.S.o: %.S
	@mkdir -p $(dir $@)
	@echo "CC: $<"
	@$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@