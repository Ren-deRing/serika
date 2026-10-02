SRCS_C := $(shell find . -name "*.c")
SRCS_S := $(shell find . -name "*.S")

OBJS := $(patsubst ./%.c,$(OBJ_DIR)/tests/%.c.o,$(SRCS_C))
OBJS += $(patsubst ./%.S,$(OBJ_DIR)/tests/%.S.o,$(SRCS_S))

CFLAGS := $(CFLAGS) -mcmodel=kernel -mno-sse -mno-sse2 -mno-mmx -mno-avx

all: $(OBJS)

$(OBJ_DIR)/tests/%.c.o: %.c
	@mkdir -p $(dir $@)
	@echo "CC: $<"
	@$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(OBJ_DIR)/tests/%.S.o: %.S
	@mkdir -p $(dir $@)
	@echo "CC: $<"
	@$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@
