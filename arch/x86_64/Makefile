SRCS_C := $(shell find . -name "*.c")
SRCS_S := $(shell find . -name "*.S")

OBJS := $(patsubst ./%.c,$(OBJ_DIR)/arch/x86_64/%.c.o,$(SRCS_C))
OBJS += $(patsubst ./%.S,$(OBJ_DIR)/arch/x86_64/%.S.o,$(SRCS_S))

CFLAGS := $(CFLAGS) -mcmodel=kernel -mno-sse -mno-sse2 -mno-mmx -mno-avx

all: $(OBJS)

$(OBJ_DIR)/arch/x86_64/%.c.o: %.c
	@mkdir -p $(dir $@)
	@echo "CC: $<"
	@$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(OBJ_DIR)/arch/x86_64/%.S.o: %.S
	@mkdir -p $(dir $@)
	@echo "CC: $<"
	@$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@
