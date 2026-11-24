CC      ?= cc
CFLAGS  = -std=c99 -Wall -Wextra -Werror -O2 -g $(EXTRA_CFLAGS)
BUILD   = build

TEST_SRCS = vm/test/test_main.c vm/test/test_term.c vm/test/test_alloc.c vm/test/test_image.c vm/test/test_heap.c vm/test/test_msg.c vm/test/test_interp.c vm/test/test_signals.c vm/test/test_timer.c vm/test/test_call.c vm/test/test_names.c vm/test/test_out.c vm/test/test_evq.c vm/test/test_port.c
# VM core + unix platform, without the unix main (shared with the C tests).
VM_SRCS   = $(wildcard vm/*.c) platform/unix/platform_unix.c platform/unix/gpio_unix.c
VM_SRCS  := $(filter-out vm/check32.c,$(VM_SRCS))
VM_CORE_SRCS = $(filter-out vm/check32.c,$(wildcard vm/*.c))
MAIN_SRCS = platform/unix/main.c

.PHONY: all vm test test-c test-c-mcu test-console test-go check32 console-ppm clean

all: test

test: test-c test-c-mcu test-console check32 test-go

vm: $(BUILD)/gogovm

$(BUILD)/gogovm: $(VM_SRCS) $(MAIN_SRCS) $(wildcard vm/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Ivm -o $@ $(VM_SRCS) $(MAIN_SRCS)

test-c: $(BUILD)/test_c
	$(BUILD)/test_c

# The same tests with the small-platform (MCU) config.
MCU_FLAGS = -DGOGO_DEFAULT_MAX_WORDS=4096u -DGOGO_TABLE_CAP=1024u -DGOGO_ARENA_BYTES=163840u

test-c-mcu: $(BUILD)/test_c_mcu
	$(BUILD)/test_c_mcu

$(BUILD)/test_c_mcu: $(TEST_SRCS) $(VM_SRCS) $(wildcard vm/*.h) $(wildcard vm/test/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(MCU_FLAGS) -Ivm -Ivm/test -o $@ $(TEST_SRCS) $(VM_SRCS)

# Console text grid + font lookup (platform/common), host build.
CONSOLE_SRCS = platform/common/console.c platform/common/font.c

test-console: $(BUILD)/test_console
	$(BUILD)/test_console

$(BUILD)/test_console: platform/common/test/test_console.c vm/test/test_main.c $(CONSOLE_SRCS) $(wildcard platform/common/*.h) platform/common/font/gogo12.font
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Iplatform/common -Ivm/test -o $@ platform/common/test/test_console.c vm/test/test_main.c $(CONSOLE_SRCS)

# Render a text file through the console grid and font into a PPM:
#   make console-ppm TEXT=platform/common/font/sample.txt OUT=build/sample.ppm
TEXT ?= platform/common/font/sample.txt
OUT  ?= $(BUILD)/console.ppm

$(BUILD)/console_ppm: platform/common/tools/console_ppm.c $(CONSOLE_SRCS) $(wildcard platform/common/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Iplatform/common -o $@ platform/common/tools/console_ppm.c $(CONSOLE_SRCS)

console-ppm: $(BUILD)/console_ppm
	$(BUILD)/console_ppm platform/common/font/gogo12.font $(TEXT) $(OUT)

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
	for f in $(CONSOLE_SRCS); do \
		clang --target=armv6m-none-eabi -ffreestanding -std=c99 -fsyntax-only -Wall -Wextra -Werror -Iplatform/common -Ivm/test/stub $$f || exit 1; \
	done

clean:
	rm -rf $(BUILD)
