CC      ?= cc
CFLAGS  = -std=c99 -Wall -Wextra -Werror -O2 -g $(EXTRA_CFLAGS)
BUILD   = build

TEST_SRCS = vm/test/test_main.c vm/test/test_term.c vm/test/test_alloc.c vm/test/test_image.c vm/test/test_heap.c vm/test/test_msg.c vm/test/test_interp.c vm/test/test_signals.c vm/test/test_timer.c vm/test/test_call.c vm/test/test_names.c
# VM core + unix platform, without the unix main (shared with the C tests).
VM_SRCS   = $(wildcard vm/*.c) platform/unix/platform_unix.c
VM_SRCS  := $(filter-out vm/check32.c,$(VM_SRCS))
VM_CORE_SRCS = $(filter-out vm/check32.c,$(wildcard vm/*.c))
MAIN_SRCS = platform/unix/main.c

.PHONY: all vm test test-c test-go check32 clean

all: test

test: test-c check32 test-go

vm: $(BUILD)/gogovm

$(BUILD)/gogovm: $(VM_SRCS) $(MAIN_SRCS) $(wildcard vm/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Ivm -o $@ $(VM_SRCS) $(MAIN_SRCS)

test-c: $(BUILD)/test_c
	$(BUILD)/test_c

test-go: vm
	GOGO_VM=$(CURDIR)/$(BUILD)/gogovm go test ./...

# Test code is host-only: it may use stdio and GCC/Clang constructors.
$(BUILD)/test_c: $(TEST_SRCS) $(VM_SRCS) $(wildcard vm/*.h) $(wildcard vm/test/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Ivm -Ivm/test -o $@ $(TEST_SRCS) $(VM_SRCS)

# Compile-check the core headers for a 32-bit target (no linking).
check32:
	for f in vm/check32.c $(VM_CORE_SRCS); do \
		clang --target=armv6m-none-eabi -ffreestanding -std=c99 -fsyntax-only -Wall -Wextra -Werror -Ivm -Ivm/test/stub $$f || exit 1; \
	done

clean:
	rm -rf $(BUILD)
