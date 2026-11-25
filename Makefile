CC      ?= cc
CFLAGS  = -Iinput -std=c99 -Wall -Wextra -Werror -O2 -g $(EXTRA_CFLAGS)
BUILD   = build

TEST_SRCS = vm/test/test_main.c vm/test/test_term.c vm/test/test_alloc.c vm/test/test_image.c vm/test/test_heap.c vm/test/test_msg.c vm/test/test_interp.c vm/test/test_signals.c vm/test/test_timer.c vm/test/test_call.c vm/test/test_names.c vm/test/test_out.c vm/test/test_evq.c vm/test/test_port.c vm/test/test_strs.c vm/test/test_slices.c vm/test/test_bytes.c vm/test/test_netlink.c vm/test/test_caps.c vm/test/test_module.c
# The net stack (pure C99, chip-independent; spec M5): linked as objects
# into every unix build, so its net_rx_offer / net_link_event replace the
# platform's weak defaults.
NET_SRCS  = $(wildcard net/*.c)
NET_TEST_SRCS = vm/test/test_main.c $(wildcard net/test/*.c)
# VM core + unix platform + net stack, without the unix main (shared with the C tests).
VM_SRCS   = $(wildcard vm/*.c) platform/unix/platform_unix.c platform/unix/gpio_unix.c platform/unix/input_unix.c $(NET_SRCS)
VM_SRCS  := $(filter-out vm/check32.c,$(VM_SRCS))
VM_CORE_SRCS = $(filter-out vm/check32.c,$(wildcard vm/*.c))
# stream/ (pure C99): Chunk / Out / Done for every streaming driver (spec input2 §1).
STREAM_SRCS = $(wildcard stream/*.c)
MAIN_SRCS = platform/unix/main.c platform/unix/fs_host.c $(wildcard vfs/*.c) $(wildcard input/*.c) $(STREAM_SRCS)
PLAT_HDRS = $(wildcard platform/unix/*.h) $(wildcard net/*.h) $(wildcard vfs/*.h) $(wildcard input/*.h) $(wildcard stream/*.h)

.PHONY: vmsize netsize all vm test test-c test-c-mcu test-net-c test-net-mcu test-vfs test-input test-stream test-fs-host vfssize test-console test-go check32 console-ppm clean

all: test

test: test-c test-c-mcu test-net-c test-vfs test-input test-stream test-fs-host test-console check32 test-go test-net-mcu vmsize netsize vfssize

vm: $(BUILD)/gogovm

$(BUILD)/gogovm: $(VM_SRCS) $(MAIN_SRCS) $(wildcard vm/*.h) $(PLAT_HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Ivm -Inet -Ivfs -Iinput -Istream -Iplatform/unix -o $@ $(VM_SRCS) $(MAIN_SRCS)

test-c: $(BUILD)/test_c
	$(BUILD)/test_c

# The same tests with the small-platform (MCU) config.
MCU_FLAGS = -DGOGO_DEFAULT_MAX_WORDS=4096u -DGOGO_TABLE_CAP=1024u -DGOGO_ARENA_BYTES=163840u

test-c-mcu: $(BUILD)/test_c_mcu
	$(BUILD)/test_c_mcu

$(BUILD)/test_c_mcu: $(TEST_SRCS) $(VM_SRCS) $(wildcard vm/*.h) $(wildcard vm/test/*.h) $(PLAT_HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(MCU_FLAGS) -Ivm -Inet -Ivm/test -Iplatform/unix -pthread -o $@ $(TEST_SRCS) $(VM_SRCS)

# Net stack C tests (parsers, malformed frames, ports), host and MCU config.
test-net-c: $(BUILD)/test_net $(BUILD)/test_net_mcu
	$(BUILD)/test_net
	$(BUILD)/test_net_mcu

$(BUILD)/test_net: $(NET_TEST_SRCS) $(VM_SRCS) $(wildcard vm/*.h) $(wildcard vm/test/*.h) $(PLAT_HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Ivm -Inet -Ivm/test -Iplatform/unix -o $@ $(NET_TEST_SRCS) $(VM_SRCS)

$(BUILD)/test_net_mcu: $(NET_TEST_SRCS) $(VM_SRCS) $(wildcard vm/*.h) $(wildcard vm/test/*.h) $(PLAT_HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(MCU_FLAGS) -Ivm -Inet -Ivm/test -Iplatform/unix -o $@ $(NET_TEST_SRCS) $(VM_SRCS)

# vfs (pure C99): path rules, slot table, rules, and the B_FS builtin
# (builtin.c, the only file that sees VM types), against the memfs fake on
# a real VM. The MCU build uses the MCU VM config, the MCU slot parameters
# (the ESP32-S3 LittleFS slot, platform/esp32s3/main/CMakeLists.txt) and -Os.
VFS_SRCS = $(wildcard vfs/*.c)
VFS_TEST_SRCS = vm/test/test_main.c $(wildcard vfs/test/*.c)
VFS_INC = -Ivfs -Ivfs/test -Ivm -Inet -Ivm/test -Iplatform/unix
VFS_DEPS = $(VFS_SRCS) $(VFS_TEST_SRCS) $(VM_SRCS) $(wildcard vfs/*.h) $(wildcard vfs/test/*.h) $(wildcard vm/*.h) $(PLAT_HDRS)

test-vfs: $(BUILD)/test_vfs $(BUILD)/test_vfs_mcu
	$(BUILD)/test_vfs
	$(BUILD)/test_vfs_mcu

$(BUILD)/test_vfs: $(VFS_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(VFS_INC) -o $@ $(VFS_SRCS) $(VFS_TEST_SRCS) $(VM_SRCS)

$(BUILD)/test_vfs_mcu: $(VFS_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(MCU_FLAGS) -Os -DGOGO_FS_MAX_OPEN=4u -DGOGO_FS_SLOT_BYTES=160u $(VFS_INC) -o $@ $(VFS_SRCS) $(VFS_TEST_SRCS) $(VM_SRCS)

# input (pure C99): the kbd port and the input source against a fake InputHw
# (input/test/fakehw.c) on a real VM, in the host and MCU VM configs.
INPUT_SRCS = $(wildcard input/*.c)
INPUT_TEST_SRCS = vm/test/test_main.c $(wildcard input/test/*.c)
INPUT_INC = -Iinput -Iinput/test -Istream -Ivm -Inet -Ivm/test -Iplatform/unix
INPUT_DEPS = $(INPUT_SRCS) $(STREAM_SRCS) $(INPUT_TEST_SRCS) $(VM_SRCS) $(wildcard input/*.h) $(wildcard input/test/*.h) $(wildcard vm/*.h) $(PLAT_HDRS)

test-input: $(BUILD)/test_input $(BUILD)/test_input_mcu
	$(BUILD)/test_input
	$(BUILD)/test_input_mcu

$(BUILD)/test_input: $(INPUT_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(INPUT_INC) -o $@ $(INPUT_SRCS) $(STREAM_SRCS) $(INPUT_TEST_SRCS) $(VM_SRCS)

$(BUILD)/test_input_mcu: $(INPUT_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(MCU_FLAGS) -Os $(INPUT_INC) -o $@ $(INPUT_SRCS) $(STREAM_SRCS) $(INPUT_TEST_SRCS) $(VM_SRCS)

# stream (pure C99): Chunk delivery and Out bookkeeping against a fake "st"
# port on a real VM (and the port-send gate through the interpreter), in the
# host and MCU VM configs.
STREAM_TEST_SRCS = vm/test/test_main.c $(wildcard stream/test/*.c)
STREAM_INC = -Istream -Ivm -Inet -Ivm/test -Iplatform/unix
STREAM_DEPS = $(STREAM_SRCS) $(STREAM_TEST_SRCS) $(VM_SRCS) $(wildcard stream/*.h) $(wildcard vm/*.h) $(wildcard vm/test/*.h) $(PLAT_HDRS)

test-stream: $(BUILD)/test_stream $(BUILD)/test_stream_mcu
	$(BUILD)/test_stream
	$(BUILD)/test_stream_mcu

$(BUILD)/test_stream: $(STREAM_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(STREAM_INC) -o $@ $(STREAM_SRCS) $(STREAM_TEST_SRCS) $(VM_SRCS)

$(BUILD)/test_stream_mcu: $(STREAM_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(MCU_FLAGS) -Os $(STREAM_INC) -o $@ $(STREAM_SRCS) $(STREAM_TEST_SRCS) $(VM_SRCS)

# unix host-directory backend against a temp dir.
test-fs-host: $(BUILD)/test_fs_host
	$(BUILD)/test_fs_host

$(BUILD)/test_fs_host: platform/unix/test/test_fs_host.c vm/test/test_main.c platform/unix/fs_host.c $(VFS_SRCS) $(VM_SRCS) $(wildcard vfs/*.h) $(PLAT_HDRS) $(wildcard vm/*.h) $(wildcard vm/test/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(VFS_INC) -o $@ platform/unix/test/test_fs_host.c vm/test/test_main.c platform/unix/fs_host.c $(VFS_SRCS) $(VM_SRCS)

# Console text grid + font lookup (platform/common), host build.
CONSOLE_SRCS = platform/common/console.c platform/common/font.c

test-console: $(BUILD)/test_console $(BUILD)/test_console_ascii
	$(BUILD)/test_console
	$(BUILD)/test_console_ascii

$(BUILD)/test_console: platform/common/test/test_console.c vm/test/test_main.c $(CONSOLE_SRCS) $(wildcard platform/common/*.h) platform/common/font/gogo12.font
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Iplatform/common -Ivm/test -o $@ platform/common/test/test_console.c vm/test/test_main.c $(CONSOLE_SRCS)

# The same tests against the ASCII-only font (GOGO_FONT_CJK=n firmware).
$(BUILD)/test_console_ascii: platform/common/test/test_console.c vm/test/test_main.c $(CONSOLE_SRCS) $(wildcard platform/common/*.h) platform/common/font/gogo12-ascii.font
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DFONT_ASCII -DFONT_PATH='"platform/common/font/gogo12-ascii.font"' -Iplatform/common -Ivm/test -o $@ platform/common/test/test_console.c vm/test/test_main.c $(CONSOLE_SRCS)

# Render a text file through the console grid and font into a PPM:
#   make console-ppm TEXT=platform/common/font/sample.txt OUT=build/sample.ppm
TEXT ?= platform/common/font/sample.txt
OUT  ?= $(BUILD)/console.ppm

$(BUILD)/console_ppm: platform/common/tools/console_ppm.c $(CONSOLE_SRCS) $(wildcard platform/common/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Iplatform/common -o $@ platform/common/tools/console_ppm.c $(CONSOLE_SRCS)

console-ppm: $(BUILD)/console_ppm
	$(BUILD)/console_ppm platform/common/font/gogo12.font $(TEXT) $(OUT)

test-go: vm $(BUILD)/gogovm_mcu
	GOGO_VM=$(CURDIR)/$(BUILD)/gogovm GOGO_VM_MCU=$(CURDIR)/$(BUILD)/gogovm_mcu go test ./...

# The network scenarios (dhcp, dns, ...) on a VM with the MCU actor memory
# limit (16 KB per actor): the library actors must fit.
$(BUILD)/gogovm_mcu: $(VM_SRCS) $(MAIN_SRCS) $(wildcard vm/*.h) $(PLAT_HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(MCU_FLAGS) -Ivm -Inet -Ivfs -Iinput -Istream -Iplatform/unix -o $@ $(VM_SRCS) $(MAIN_SRCS)

test-net-mcu: $(BUILD)/gogovm_mcu
	GOGO_VM=$(CURDIR)/$(BUILD)/gogovm_mcu go test -count=1 ./tests/net/

# Test code is host-only: it may use stdio and GCC/Clang constructors.
$(BUILD)/test_c: $(TEST_SRCS) $(VM_SRCS) $(wildcard vm/*.h) $(wildcard vm/test/*.h) $(PLAT_HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Ivm -Inet -Ivm/test -Iplatform/unix -pthread -o $@ $(TEST_SRCS) $(VM_SRCS)

# Compile-check the core headers for a 32-bit target (no linking).
check32:
	for f in vm/check32.c $(VM_CORE_SRCS); do \
		clang --target=armv6m-none-eabi -ffreestanding -std=c99 -fsyntax-only -Wall -Wextra -Werror -Ivm -Ivm/test/stub $$f || exit 1; \
	done
	for f in $(NET_SRCS); do \
		clang --target=armv6m-none-eabi -ffreestanding -std=c99 -fsyntax-only -Wall -Wextra -Werror -Ivm -Inet -Ivm/test/stub $$f || exit 1; \
	done
	for f in $(STREAM_SRCS); do \
		clang --target=armv6m-none-eabi -ffreestanding -std=c99 -fsyntax-only -Wall -Wextra -Werror -Istream -Ivm -Ivm/test/stub $$f || exit 1; \
	done
	for f in $(VFS_SRCS); do \
		clang --target=armv6m-none-eabi -ffreestanding -std=c99 -fsyntax-only -Wall -Wextra -Werror -Ivfs -Ivm -Ivm/test/stub $$f || exit 1; \
	done
	for f in $(CONSOLE_SRCS); do \
		clang --target=armv6m-none-eabi -ffreestanding -std=c99 -fsyntax-only -Wall -Wextra -Werror -Iplatform/common -Ivm/test/stub $$f || exit 1; \
	done

clean:
	rm -rf $(BUILD)

# VM core size on the ESP32-S3 (-Os); fails above 64 KB, skips without the toolchain.
vmsize:
	tools/vmsize.sh

# net/ code size and fixed state on the ESP32-S3 (-Os), reported apart from the VM core.
netsize:
	tools/vmsize.sh --net

# vfs/ code size and fixed state on the ESP32-S3 (-Os), reported apart from the VM core.
vfssize:
	tools/vmsize.sh --vfs
