CC      ?= cc
# The wall-clock floor (runtime/vm/host.h): the time of this make run.
BUILD_UNIX_MS := $(shell date +%s)000
CFLAGS  = -Iruntime/input -std=c99 -Wall -Wextra -Werror -O2 -g -DGOGO_BUILD_UNIX_MS=$(BUILD_UNIX_MS)LL $(EXTRA_CFLAGS)
BUILD   = build

TEST_SRCS = runtime/vm/test/test_main.c runtime/vm/test/test_host.c runtime/vm/test/test_term.c runtime/vm/test/test_alloc.c runtime/vm/test/test_image.c runtime/vm/test/test_heap.c runtime/vm/test/test_msg.c runtime/vm/test/test_interp.c runtime/vm/test/test_signals.c runtime/vm/test/test_timer.c runtime/vm/test/test_call.c runtime/vm/test/test_names.c runtime/vm/test/test_out.c runtime/vm/test/test_evq.c runtime/vm/test/test_port.c runtime/vm/test/test_strs.c runtime/vm/test/test_slices.c runtime/vm/test/test_bytes.c runtime/vm/test/test_netlink.c runtime/vm/test/test_caps.c runtime/vm/test/test_module.c runtime/vm/test/test_walltime.c
# The net stack (pure C99, chip-independent): linked into every
# unix build (platform_unix.c lists its drivers and feeds it frames).
NET_SRCS  = $(wildcard runtime/net/*.c)
NET_TEST_SRCS = runtime/vm/test/test_main.c runtime/vm/test/test_host.c $(wildcard runtime/net/test/*.c)
# VM core + unix platform + net stack, without the unix main (shared with the C tests).
VM_SRCS   = $(wildcard runtime/vm/*.c) platform/unix/platform_unix.c platform/unix/gpio_unix.c platform/unix/input_unix.c $(NET_SRCS)
VM_SRCS  := $(filter-out runtime/vm/check32.c,$(VM_SRCS))
VM_CORE_SRCS = $(filter-out runtime/vm/check32.c,$(wildcard runtime/vm/*.c))
# runtime/stream/ (pure C99): Chunk / Out / Done for every streaming driver (spec input2 §1).
STREAM_SRCS = $(wildcard runtime/stream/*.c)
MAIN_SRCS = platform/unix/main.c platform/unix/fs_host.c $(wildcard runtime/vfs/*.c) $(wildcard runtime/input/*.c) $(STREAM_SRCS)
PLAT_HDRS = $(wildcard platform/unix/*.h) $(wildcard runtime/net/*.h) $(wildcard runtime/vfs/*.h) $(wildcard runtime/input/*.h) $(wildcard runtime/stream/*.h)

# The "tls" port (runtime/net/tls/, spec HTTPS): mbedTLS 4.x from ESP-IDF's source tree,
# built here with runtime/net/tls/config (gogo_*_config.h) into its own static library,
# linked into gogovm / gogovm_mcu only (never the VM core: `make vmsize` is
# unaffected; `make tlssize` reports it). Without the source tree gogovm is
# built without the port (and the tls tests skip).
MBEDTLS_DIR ?= $(HOME)/esp/esp-idf/components/mbedtls/mbedtls
ifneq ($(wildcard $(MBEDTLS_DIR)/library/ssl_tls.c),)
MBEDTLS_TF   = $(MBEDTLS_DIR)/tf-psa-crypto
MBEDTLS_SRCS = $(wildcard $(MBEDTLS_DIR)/library/*.c) $(wildcard $(MBEDTLS_TF)/core/*.c) \
               $(wildcard $(MBEDTLS_TF)/drivers/builtin/src/*.c) $(wildcard $(MBEDTLS_TF)/extras/*.c) \
               $(wildcard $(MBEDTLS_TF)/platform/*.c) $(wildcard $(MBEDTLS_TF)/utilities/*.c)
MBEDTLS_OBJS = $(patsubst $(MBEDTLS_DIR)/%.c,$(BUILD)/mbedtls/%.o,$(MBEDTLS_SRCS))
MBEDTLS_CFG  = -Iruntime/net/tls/config -DMBEDTLS_CONFIG_FILE='"gogo_mbedtls_config.h"' -DTF_PSA_CRYPTO_CONFIG_FILE='"gogo_crypto_config.h"'
MBEDTLS_PUB  = $(MBEDTLS_CFG) -I$(MBEDTLS_DIR)/include -I$(MBEDTLS_TF)/include -I$(MBEDTLS_TF)/drivers/builtin/include
MBEDTLS_PRIV = $(MBEDTLS_PUB) -Iruntime/net/tls/host -I$(MBEDTLS_DIR)/library -I$(MBEDTLS_TF)/core -I$(MBEDTLS_TF)/drivers/builtin/src \
               -I$(MBEDTLS_TF)/dispatch -I$(MBEDTLS_TF)/utilities -I$(MBEDTLS_TF)/platform -I$(MBEDTLS_TF)/extras
MBEDTLS_LIB  = $(BUILD)/libgogo_mbedtls.a
TLS_SRCS     = runtime/net/tls/tls.c runtime/net/tls/anchor.c platform/unix/tls_unix.c
TLS_CFLAGS   = -DGOGO_TLS -Iruntime/net/tls $(MBEDTLS_PUB)
TLS_LINK     = $(TLS_SRCS) $(MBEDTLS_LIB)
TLS_DEPS     = $(TLS_SRCS) $(wildcard runtime/net/tls/*.h) $(wildcard runtime/net/tls/config/*.h) $(MBEDTLS_LIB)

$(BUILD)/mbedtls/%.o: $(MBEDTLS_DIR)/%.c $(wildcard runtime/net/tls/config/*.h)
	@mkdir -p $(dir $@)
	@$(CC) -std=c99 -O2 -g -w $(EXTRA_CFLAGS) $(MBEDTLS_PRIV) -c $< -o $@

$(MBEDTLS_LIB): $(MBEDTLS_OBJS)
	@rm -f $@
	ar rcs $@ $(MBEDTLS_OBJS)
endif

.PHONY: vmsize netsize tlssize test-tls-asan all vm test test-c test-c-mcu test-net-c test-tls-c test-net-mcu test-vfs test-input test-stream test-fs-host vfssize test-console test-go test-lib check32 globals globals-selftest console-ppm clean

all: test

test: test-c test-c-mcu test-net-c test-tls-c test-vfs test-input test-stream test-fs-host test-console check32 globals globals-selftest test-go test-net-mcu vmsize netsize vfssize

vm: $(BUILD)/gogovm

$(BUILD)/gogovm: $(VM_SRCS) $(MAIN_SRCS) $(wildcard runtime/vm/*.h) $(PLAT_HDRS) $(TLS_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(TLS_CFLAGS) -Iruntime/vm -Iruntime/net -Iruntime/vfs -Iruntime/input -Iruntime/stream -Iplatform/unix -o $@ $(VM_SRCS) $(MAIN_SRCS) $(TLS_LINK)

test-c: $(BUILD)/test_c
	$(BUILD)/test_c

# The same tests with the small-platform (MCU) config.
MCU_FLAGS = -DGOGO_DEFAULT_MAX_WORDS=4096u -DGOGO_TABLE_CAP=1024u -DGOGO_ARENA_BYTES=163840u

test-c-mcu: $(BUILD)/test_c_mcu
	$(BUILD)/test_c_mcu

$(BUILD)/test_c_mcu: $(TEST_SRCS) $(VM_SRCS) $(wildcard runtime/vm/*.h) $(wildcard runtime/vm/test/*.h) $(PLAT_HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(MCU_FLAGS) -Iruntime/vm -Iruntime/net -Iruntime/vm/test -Iplatform/unix -pthread -o $@ $(TEST_SRCS) $(VM_SRCS)

# Net stack C tests (parsers, malformed frames, ports), host and MCU config.
test-net-c: $(BUILD)/test_net $(BUILD)/test_net_mcu
	$(BUILD)/test_net
	$(BUILD)/test_net_mcu

$(BUILD)/test_net: $(NET_TEST_SRCS) $(VM_SRCS) $(wildcard runtime/vm/*.h) $(wildcard runtime/vm/test/*.h) $(PLAT_HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Iruntime/vm -Iruntime/net -Iruntime/vm/test -Iplatform/unix -o $@ $(NET_TEST_SRCS) $(VM_SRCS)

# The "tls" port's per-stack state (two Vms, two connections), against the
# mbedTLS library; skipped without the mbedTLS source tree.
TLS_TEST_SRCS = runtime/vm/test/test_main.c runtime/vm/test/test_host.c $(wildcard runtime/net/tls/test/*.c)
ifneq ($(TLS_CFLAGS),)
test-tls-c: $(BUILD)/test_tls
	$(BUILD)/test_tls
else
test-tls-c:
	@echo "test-tls-c: skipped (no mbedTLS source at $(MBEDTLS_DIR))"
endif

$(BUILD)/test_tls: $(TLS_TEST_SRCS) $(VM_SRCS) $(wildcard runtime/vm/*.h) $(wildcard runtime/vm/test/*.h) $(PLAT_HDRS) $(TLS_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(TLS_CFLAGS) -Iruntime/vm -Iruntime/net -Iruntime/vm/test -Iplatform/unix -o $@ $(TLS_TEST_SRCS) $(VM_SRCS) $(TLS_LINK)

$(BUILD)/test_net_mcu: $(NET_TEST_SRCS) $(VM_SRCS) $(wildcard runtime/vm/*.h) $(wildcard runtime/vm/test/*.h) $(PLAT_HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(MCU_FLAGS) -Iruntime/vm -Iruntime/net -Iruntime/vm/test -Iplatform/unix -o $@ $(NET_TEST_SRCS) $(VM_SRCS)

# vfs (pure C99): path rules, slot table, rules, and the B_FS builtin
# (builtin.c, the only file that sees VM types), against the memfs fake on
# a real VM. The MCU build uses the MCU VM config, the MCU slot parameters
# (the ESP32-S3 LittleFS slot, platform/esp32s3/main/CMakeLists.txt) and -Os.
VFS_SRCS = $(wildcard runtime/vfs/*.c)
VFS_TEST_SRCS = runtime/vm/test/test_main.c runtime/vm/test/test_host.c $(wildcard runtime/vfs/test/*.c)
VFS_INC = -Iruntime/vfs -Iruntime/vfs/test -Iruntime/vm -Iruntime/net -Iruntime/vm/test -Iplatform/unix
VFS_DEPS = $(VFS_SRCS) $(VFS_TEST_SRCS) $(VM_SRCS) $(wildcard runtime/vfs/*.h) $(wildcard runtime/vfs/test/*.h) $(wildcard runtime/vm/*.h) $(PLAT_HDRS)

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
# (runtime/input/test/fakehw.c) on a real VM, in the host and MCU VM configs.
INPUT_SRCS = $(wildcard runtime/input/*.c)
INPUT_TEST_SRCS = runtime/vm/test/test_main.c runtime/vm/test/test_host.c $(wildcard runtime/input/test/*.c)
INPUT_INC = -Iruntime/input -Iruntime/input/test -Iruntime/stream -Iruntime/vm -Iruntime/net -Iruntime/vm/test -Iplatform/unix
INPUT_DEPS = $(INPUT_SRCS) $(STREAM_SRCS) $(INPUT_TEST_SRCS) $(VM_SRCS) $(wildcard runtime/input/*.h) $(wildcard runtime/input/test/*.h) $(wildcard runtime/vm/*.h) $(PLAT_HDRS)

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
STREAM_TEST_SRCS = runtime/vm/test/test_main.c runtime/vm/test/test_host.c $(wildcard runtime/stream/test/*.c)
STREAM_INC = -Iruntime/stream -Iruntime/vm -Iruntime/net -Iruntime/vm/test -Iplatform/unix
STREAM_DEPS = $(STREAM_SRCS) $(STREAM_TEST_SRCS) $(VM_SRCS) $(wildcard runtime/stream/*.h) $(wildcard runtime/vm/*.h) $(wildcard runtime/vm/test/*.h) $(PLAT_HDRS)

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

$(BUILD)/test_fs_host: platform/unix/test/test_fs_host.c runtime/vm/test/test_main.c runtime/vm/test/test_host.c platform/unix/fs_host.c $(VFS_SRCS) $(VM_SRCS) $(wildcard runtime/vfs/*.h) $(PLAT_HDRS) $(wildcard runtime/vm/*.h) $(wildcard runtime/vm/test/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(VFS_INC) -o $@ platform/unix/test/test_fs_host.c runtime/vm/test/test_main.c runtime/vm/test/test_host.c platform/unix/fs_host.c $(VFS_SRCS) $(VM_SRCS)

# Console text grid + font lookup (runtime/console), host build.
CONSOLE_SRCS = runtime/console/console.c runtime/console/font.c

test-console: $(BUILD)/test_console $(BUILD)/test_console_ascii
	$(BUILD)/test_console
	$(BUILD)/test_console_ascii

$(BUILD)/test_console: runtime/console/test/test_console.c runtime/vm/test/test_main.c $(CONSOLE_SRCS) $(wildcard runtime/console/*.h) runtime/console/font/gogo12.font
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Iruntime/console -Iruntime/vm/test -o $@ runtime/console/test/test_console.c runtime/vm/test/test_main.c $(CONSOLE_SRCS)

# The same tests against the ASCII-only font (GOGO_FONT_CJK=n firmware).
$(BUILD)/test_console_ascii: runtime/console/test/test_console.c runtime/vm/test/test_main.c $(CONSOLE_SRCS) $(wildcard runtime/console/*.h) runtime/console/font/gogo12-ascii.font
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DFONT_ASCII -DFONT_PATH='"runtime/console/font/gogo12-ascii.font"' -Iruntime/console -Iruntime/vm/test -o $@ runtime/console/test/test_console.c runtime/vm/test/test_main.c $(CONSOLE_SRCS)

# Render a text file through the console grid and font into a PPM:
#   make console-ppm TEXT=runtime/console/font/sample.txt OUT=build/sample.ppm
TEXT ?= runtime/console/font/sample.txt
OUT  ?= $(BUILD)/console.ppm

$(BUILD)/console_ppm: runtime/console/tools/console_ppm.c $(CONSOLE_SRCS) $(wildcard runtime/console/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Iruntime/console -o $@ runtime/console/tools/console_ppm.c $(CONSOLE_SRCS)

console-ppm: $(BUILD)/console_ppm
	$(BUILD)/console_ppm runtime/console/font/gogo12.font $(TEXT) $(OUT)

test-go: vm $(BUILD)/gogovm_mcu test-lib
	GOGO_VM=$(CURDIR)/$(BUILD)/gogovm GOGO_VM_MCU=$(CURDIR)/$(BUILD)/gogovm_mcu go test ./...

# The standard library (lib/, its own module gogo) on the host toolchain:
# vet (stdmethods off: gogo's bytes.Buffer.WriteByte deliberately returns no
# error) and its host tests (gogo/irproto).
test-lib:
	cd lib && go vet -stdmethods=false ./... && go test ./...

# The network scenarios (dhcp, dns, ...) on a VM with the MCU actor memory
# limit (16 KB per actor): the library actors must fit.
$(BUILD)/gogovm_mcu: $(VM_SRCS) $(MAIN_SRCS) $(wildcard runtime/vm/*.h) $(PLAT_HDRS) $(TLS_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(MCU_FLAGS) $(TLS_CFLAGS) -Iruntime/vm -Iruntime/net -Iruntime/vfs -Iruntime/input -Iruntime/stream -Iplatform/unix -o $@ $(VM_SRCS) $(MAIN_SRCS) $(TLS_LINK)

test-net-mcu: $(BUILD)/gogovm_mcu
	GOGO_VM=$(CURDIR)/$(BUILD)/gogovm_mcu go test -count=1 ./tests/net/

# The net and tls scenarios on a gogovm (and its mbedTLS) built with
# AddressSanitizer (build/asan). On macOS leaks are not reported (no
# LeakSanitizer); the tls tests check the TLS heap returns to 0 instead.
ASAN_FLAGS = -fsanitize=address -fno-omit-frame-pointer
test-tls-asan:
	$(MAKE) BUILD=build/asan EXTRA_CFLAGS="$(ASAN_FLAGS)" build/asan/gogovm build/asan/test_net build/asan/test_tls
	build/asan/test_net
	build/asan/test_tls
	GOGO_VM=$(CURDIR)/build/asan/gogovm go test -count=1 ./tests/net/

# Test code is host-only: it may use stdio and GCC/Clang constructors.
$(BUILD)/test_c: $(TEST_SRCS) $(VM_SRCS) $(wildcard runtime/vm/*.h) $(wildcard runtime/vm/test/*.h) $(PLAT_HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Iruntime/vm -Iruntime/net -Iruntime/vm/test -Iplatform/unix -pthread -o $@ $(TEST_SRCS) $(VM_SRCS)

# Compile-check the core headers for a 32-bit target (no linking).
check32:
	for f in runtime/vm/check32.c $(VM_CORE_SRCS); do \
		clang --target=armv6m-none-eabi -ffreestanding -std=c99 -fsyntax-only -Wall -Wextra -Werror -Iruntime/vm -Iruntime/vm/test/stub $$f || exit 1; \
	done
	for f in $(NET_SRCS); do \
		clang --target=armv6m-none-eabi -ffreestanding -std=c99 -fsyntax-only -Wall -Wextra -Werror -Iruntime/vm -Iruntime/net -Iruntime/vm/test/stub $$f || exit 1; \
	done
	for f in $(STREAM_SRCS); do \
		clang --target=armv6m-none-eabi -ffreestanding -std=c99 -fsyntax-only -Wall -Wextra -Werror -Iruntime/stream -Iruntime/vm -Iruntime/vm/test/stub $$f || exit 1; \
	done
	for f in $(VFS_SRCS); do \
		clang --target=armv6m-none-eabi -ffreestanding -std=c99 -fsyntax-only -Wall -Wextra -Werror -Iruntime/vfs -Iruntime/vm -Iruntime/vm/test/stub $$f || exit 1; \
	done
	for f in $(CONSOLE_SRCS); do \
		clang --target=armv6m-none-eabi -ffreestanding -std=c99 -fsyntax-only -Wall -Wextra -Werror -Iruntime/console -Iruntime/vm/test/stub $$f || exit 1; \
	done

# No mutable globals in runtime/ (embed mode): tools/globals.sh compiles it with the VM build's CC/CFLAGS.
globals: export GLOBALS_TLS_CFLAGS = $(TLS_CFLAGS)
globals:
	GLOBALS_CC='$(CC)' GLOBALS_CFLAGS='$(CFLAGS)' GLOBALS_MCU_FLAGS='$(MCU_FLAGS)' tools/globals.sh

# The guard must flag exactly the mutable symbols of its fixture (and no const ones).
globals-selftest:
	GLOBALS_CC='$(CC)' GLOBALS_CFLAGS='$(CFLAGS)' GLOBALS_MCU_FLAGS='$(MCU_FLAGS)' tools/globals.sh --print tools/selftest/globals_fixture.c | diff - tools/selftest/globals_fixture.expect

clean:
	rm -rf $(BUILD)

# VM core size on the ESP32-S3 (-Os); fails above 64 KB, skips without the toolchain.
vmsize:
	tools/vmsize.sh

# runtime/net/ code size and fixed state on the ESP32-S3 (-Os), reported apart from the VM core.
netsize:
	tools/vmsize.sh --net

# runtime/net/tls/ plus the mbedTLS it links, on the ESP32-S3 (-Os), reported apart from the VM core.
tlssize:
	MBEDTLS_DIR=$(MBEDTLS_DIR) tools/vmsize.sh --tls

# runtime/vfs/ code size and fixed state on the ESP32-S3 (-Os), reported apart from the VM core.
vfssize:
	tools/vmsize.sh --vfs
