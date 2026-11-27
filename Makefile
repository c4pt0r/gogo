CC      ?= cc
# The wall-clock floor (runtime/vm/host.h): the time of this make run.
BUILD_UNIX_MS := $(shell date +%s)000
CFLAGS_BASE = -Iruntime/input -std=c99 -Wall -Wextra -Werror -O2 -g
CFLAGS  = $(CFLAGS_BASE) -DGOGO_BUILD_UNIX_MS=$(BUILD_UNIX_MS)LL $(EXTRA_CFLAGS)
BUILD   = build

# $(BUILD)/.cflags holds the compiler and flags the build dir was built
# with (the wall-clock floor aside); it is rewritten when they change, and
# every binary and object depends on it (STAMPED, below), so switching
# EXTRA_CFLAGS on a build dir (build/asan, build/tsan, ...) rebuilds it.
FLAGS_STAMP = $(BUILD)/.cflags
FLAGS_NOW = $(CC) $(CFLAGS_BASE) $(EXTRA_CFLAGS)
$(FLAGS_STAMP): FORCE
	@mkdir -p $(BUILD)
	@if [ "$$(cat $@ 2>/dev/null)" != '$(FLAGS_NOW)' ]; then echo '$(FLAGS_NOW)' > $@; fi
.PHONY: FORCE
FORCE:

TEST_SRCS = runtime/vm/test/test_main.c runtime/vm/test/test_host.c runtime/vm/test/test_term.c runtime/vm/test/test_alloc.c runtime/vm/test/test_image.c runtime/vm/test/test_heap.c runtime/vm/test/test_msg.c runtime/vm/test/test_interp.c runtime/vm/test/test_signals.c runtime/vm/test/test_timer.c runtime/vm/test/test_call.c runtime/vm/test/test_names.c runtime/vm/test/test_out.c runtime/vm/test/test_evq.c runtime/vm/test/test_port.c runtime/vm/test/test_strs.c runtime/vm/test/test_slices.c runtime/vm/test/test_bytes.c runtime/vm/test/test_netlink.c runtime/vm/test/test_module.c runtime/vm/test/test_walltime.c runtime/vm/test/test_step.c runtime/vm/test/test_float.c runtime/vm/test/test_params.c $(PARAMS_SRCS)
# The VM's parameters (runtime/board/params.c: resolution, keys, checks; not
# VM core): every platform that boots a VM links it.
PARAMS_SRCS = runtime/board/params.c
# The net stack (pure C99, chip-independent): linked into every
# unix build (platform_unix.c lists its drivers and feeds it frames).
NET_SRCS  = $(wildcard runtime/net/*.c)
# runtime/stream/ (pure C99): the stream protocol (Chunk / Out / Done) of every byte port.
STREAM_SRCS = $(wildcard runtime/stream/*.c)
NET_TEST_SRCS = runtime/vm/test/test_main.c runtime/vm/test/test_host.c $(wildcard runtime/net/test/*.c)
# runtime/floatlib/ (C99): the float library (gogo/math, strconv.FormatFloat
# and ParseFloat), linked wherever the VM core is (it is not VM core: `make
# vmsize` does not count it). Empty when built with -DGOGO_FLOAT=0.
FLOATLIB_SRCS = $(wildcard runtime/floatlib/*.c)
# VM core + unix platform + net stack and the stream protocol it speaks, without the unix main (shared with the C tests).
VM_SRCS   = $(wildcard runtime/vm/*.c) $(FLOATLIB_SRCS) platform/unix/platform_unix.c runtime/gpio/gpio.c platform/unix/gpio_unix.c platform/unix/input_unix.c $(NET_SRCS) $(STREAM_SRCS)
VM_SRCS  := $(filter-out runtime/vm/check32.c,$(VM_SRCS))
VM_CORE_SRCS = $(filter-out runtime/vm/check32.c,$(wildcard runtime/vm/*.c))
# runtime/screen/ (pure C99): the "screen" port's grid and driver; render.c
# (pixels, for the LCD) is not in gogovm, which draws on the terminal
# (platform/unix/screen_unix.c). The grid's width rule is the console's.
SCREEN_SRCS = runtime/screen/screen.c runtime/screen/screen_port.c runtime/console/console.c runtime/console/font.c
# runtime/dist/ (pure C99): the "node" port of distributed actors; hostnet.c:
# the "tcp" and "tcp_listen" ports on host sockets (gogovm --host-net).
DIST_SRCS = $(wildcard runtime/dist/*.c)
MAIN_SRCS = platform/unix/main.c platform/unix/screen_unix.c platform/unix/hostnet.c runtime/board/board_main.c $(PARAMS_SRCS) runtime/embed/fs_dir.c $(wildcard runtime/vfs/*.c) $(wildcard runtime/input/*.c) $(SCREEN_SRCS) $(DIST_SRCS)
PLAT_HDRS = $(wildcard runtime/dist/*.h) $(wildcard platform/unix/*.h) runtime/gpio/gpio.h runtime/board/board_main.h runtime/embed/fs_dir.h $(wildcard runtime/net/*.h) $(wildcard runtime/vfs/*.h) $(wildcard runtime/input/*.h) $(wildcard runtime/stream/*.h) $(wildcard runtime/screen/*.h) $(wildcard runtime/console/*.h)

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
TLS_CFLAGS   = -DGOGO_TLS -pthread -Iruntime/net/tls $(MBEDTLS_PUB)
TLS_LINK     = $(TLS_SRCS) $(MBEDTLS_LIB)
TLS_DEPS     = $(TLS_SRCS) $(wildcard runtime/net/tls/*.h) $(wildcard runtime/net/tls/config/*.h) $(MBEDTLS_LIB)

$(BUILD)/mbedtls/%.o: $(MBEDTLS_DIR)/%.c $(wildcard runtime/net/tls/config/*.h)
	@mkdir -p $(dir $@)
	@$(CC) -std=c99 -O2 -g -w $(EXTRA_CFLAGS) $(MBEDTLS_PRIV) -c $< -o $@

$(MBEDTLS_LIB): $(MBEDTLS_OBJS)
	@rm -f $@
	ar rcs $@ $(MBEDTLS_OBJS)
endif

.PHONY: test-dist-c test-dist-scale test-dist-san distsize float-corpus lint gogo-cmd test-examples test-book site site-check lib libgogoc test-libgogo test-gogoc test-gobind example-embed example-driver test-embed test-embed-tsan test-embed-leaks embed-images vmsize netsize tlssize test-tls-asan test-tls-threads test-tls-tsan all vm test test-c test-c-mcu test-net-c test-tls-c test-net-mcu test-vfs test-input test-stream test-screen test-screen-asan test-fs-host test-configsvc vfssize screensize floatlibsize test-console test-go test-lib check32 globals globals-selftest console-ppm bytecode-doc clean

all: test

test: lint test-c test-c-mcu test-dist-c test-net-c test-tls-c test-tls-threads test-vfs test-input test-stream test-screen test-fs-host test-configsvc test-gpio test-fat test-lfs test-console test-embed test-libgogo test-gogoc test-gobind example-embed example-driver check32 globals globals-selftest test-go test-examples test-book site-check test-net-mcu vmsize netsize vfssize screensize floatlibsize distsize

vm: $(BUILD)/gogovm

# gofmt on .go and .gg files, go vet, shellcheck on tools/*.sh when installed (tools/lint.sh).
lint:
	tools/lint.sh

# Every examples/*/ and platform/esp32s3/examples/*/ program builds; the ones
# with an expected.out run with gogo run and print it (tools/examples.sh).
test-examples: vm $(BUILD)/gogovm_mcu gogo-cmd
	tools/examples.sh $(BUILD)

# Every example in the book runs and prints what the book shows
# (docs/book/examples/run.sh, with this build's gogo and gogovm).
test-book: vm gogo-cmd
	GOGO_CMD=$(CURDIR)/$(BUILD)/gogo GOGO_VM=$(CURDIR)/$(BUILD)/gogovm docs/book/examples/run.sh

# The website, plain files with relative links: the landing page
# (site/index.html) as $(BUILD)/site/index.html and the book built by
# mdBook (docs/book/book.toml, SUMMARY.md, theme/) in $(BUILD)/site/book.
# mdBook copies every non-markdown file of docs/book; the example programs
# and book.toml are removed again (the book links examples/ on GitHub).
# The brand marks come from logos/: the mascot for the landing page, the
# Gogo Lang lockup for the book's sidebar (theme/gogo.css); the favicon is
# the book's (docs/book/theme/favicon.*), shared by the landing page.
SITE_LOGOS = logos/gogo-octopus-color.svg
BOOK_LOGOS = logos/gogo-lang-color.svg
MDBOOK ?= mdbook
SITE = $(BUILD)/site
MDBOOK_HINT = install mdBook 0.5: brew install mdbook, or a release binary from https://github.com/rust-lang/mdBook/releases
site:
	@command -v $(MDBOOK) >/dev/null 2>&1 || { echo "make site: $(MDBOOK) not found; $(MDBOOK_HINT)" >&2; exit 1; }
	rm -rf $(SITE)
	$(MDBOOK) build docs/book -d $(CURDIR)/$(SITE)/book
	rm -rf $(SITE)/book/examples $(SITE)/book/book.toml
	cp site/index.html $(SITE)/index.html
	cp -R site/.well-known $(SITE)/.well-known
	mkdir -p $(SITE)/logos $(SITE)/book/logos
	cp $(SITE_LOGOS) $(SITE)/logos/
	cp $(BOOK_LOGOS) $(SITE)/book/logos/
	cp docs/book/theme/favicon.svg docs/book/theme/favicon.png $(SITE)/
	@echo "site: open $(SITE)/index.html"

# make site, then every relative link and #anchor of the site resolves and
# no markdown is left unconverted (tools/sitecheck); skipped without mdbook.
site-check:
	@if command -v $(MDBOOK) >/dev/null 2>&1; then \
	    $(MAKE) --no-print-directory site && go run ./tools/sitecheck $(SITE); \
	else \
	    echo "site-check: skipped: mdbook not installed ($(MDBOOK_HINT))"; \
	fi

# The gogo command, one rule for every target that needs it, so that
# targets run in parallel (make -j) do not write it at the same time
# (go build itself decides whether it is stale).
gogo-cmd:
	@mkdir -p $(BUILD)
	go build -o $(BUILD)/gogo ./cmd/gogo

$(BUILD)/gogovm: $(VM_SRCS) $(MAIN_SRCS) $(wildcard runtime/vm/*.h) $(PLAT_HDRS) $(TLS_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(TLS_CFLAGS) -Iruntime/vm -Iruntime/net -Iruntime/vfs -Iruntime/input -Iruntime/stream -Iruntime/screen -Iruntime/console -Iruntime/embed -Iruntime/gpio -Iruntime/board -Iruntime/dist -Iplatform/unix -o $@ $(VM_SRCS) $(MAIN_SRCS) $(TLS_LINK)

test-c: $(BUILD)/test_c
	$(BUILD)/test_c

# The same tests with the small-platform (MCU) config.
MCU_FLAGS = -DGOGO_DEFAULT_MAX_WORDS=4096u -DGOGO_TABLE_CAP=1024u -DGOGO_ARENA_BYTES=163840u

test-c-mcu: $(BUILD)/test_c_mcu
	$(BUILD)/test_c_mcu

$(BUILD)/test_c_mcu: $(TEST_SRCS) $(VM_SRCS) $(wildcard runtime/vm/*.h) $(wildcard runtime/vm/test/*.h) $(PLAT_HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(MCU_FLAGS) -Iruntime/vm -Iruntime/net -Iruntime/stream -Iruntime/vm/test -Iruntime/gpio -Iruntime/board -Iplatform/unix -pthread -o $@ $(TEST_SRCS) $(VM_SRCS)

# Net stack C tests (parsers, malformed frames, ports), host and MCU config.
test-net-c: $(BUILD)/test_net $(BUILD)/test_net_mcu
	$(BUILD)/test_net
	$(BUILD)/test_net_mcu

$(BUILD)/test_net: $(NET_TEST_SRCS) $(VM_SRCS) $(wildcard runtime/vm/*.h) $(wildcard runtime/vm/test/*.h) $(PLAT_HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Iruntime/vm -Iruntime/net -Iruntime/stream -Iruntime/vm/test -Iruntime/gpio -Iplatform/unix -o $@ $(NET_TEST_SRCS) $(VM_SRCS)

# The "tls" port's per-stack state (two Vms, two connections), against the
# mbedTLS library; skipped without the mbedTLS source tree.
TLS_TEST_SRCS = runtime/vm/test/test_main.c runtime/vm/test/test_host.c runtime/net/tls/test/test_tls.c
ifneq ($(TLS_CFLAGS),)
test-tls-c: $(BUILD)/test_tls
	$(BUILD)/test_tls

# Two Vms on two threads, TLS handshakes at once with different wall clocks
# (tls_threads.c), against a real TLS server the Go test starts; under
# ThreadSanitizer with test-tls-tsan.
test-tls-threads: $(BUILD)/test_tls_threads
	GOGO_TLS_THREADS=$(CURDIR)/$(BUILD)/test_tls_threads go test -count=1 -run '^TestTLSThreads$$' ./tests/net/
test-tls-tsan:
	$(MAKE) BUILD=build/tsan EXTRA_CFLAGS="-fsanitize=thread" build/tsan/test_tls_threads
	TSAN_OPTIONS="halt_on_error=1 suppressions=$(CURDIR)/runtime/net/tls/test/tsan.supp" \
	    GOGO_TLS_THREADS=$(CURDIR)/build/tsan/test_tls_threads go test -count=1 -run '^TestTLSThreads$$' ./tests/net/
else
test-tls-c:
	@echo "test-tls-c: skipped (no mbedTLS source at $(MBEDTLS_DIR))"
test-tls-threads test-tls-tsan:
	@echo "$@: skipped (no mbedTLS source at $(MBEDTLS_DIR))"
endif

$(BUILD)/test_tls_threads: runtime/net/tls/test/tls_threads.c $(VM_SRCS) $(wildcard runtime/vm/*.h) $(PLAT_HDRS) $(TLS_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(TLS_CFLAGS) -Iruntime/vm -Iruntime/net -Iruntime/stream -Iruntime/vm/test -Iruntime/gpio -Iplatform/unix -o $@ runtime/net/tls/test/tls_threads.c $(VM_SRCS) $(TLS_LINK)

$(BUILD)/test_tls: $(TLS_TEST_SRCS) $(VM_SRCS) $(wildcard runtime/vm/*.h) $(wildcard runtime/vm/test/*.h) $(PLAT_HDRS) $(TLS_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(TLS_CFLAGS) -Iruntime/vm -Iruntime/net -Iruntime/stream -Iruntime/vm/test -Iruntime/gpio -Iplatform/unix -o $@ $(TLS_TEST_SRCS) $(VM_SRCS) $(TLS_LINK)

$(BUILD)/test_net_mcu: $(NET_TEST_SRCS) $(VM_SRCS) $(wildcard runtime/vm/*.h) $(wildcard runtime/vm/test/*.h) $(PLAT_HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(MCU_FLAGS) -Iruntime/vm -Iruntime/net -Iruntime/stream -Iruntime/vm/test -Iruntime/gpio -Iplatform/unix -o $@ $(NET_TEST_SRCS) $(VM_SRCS)

# vfs (pure C99): path rules, slot table, rules, and the B_FS builtin
# (builtin.c, the only file that sees VM types), against the memfs fake on
# a real VM. The MCU build uses the MCU VM config, the MCU slot parameters
# (the ESP32-S3 LittleFS slot, platform/esp32s3/main/CMakeLists.txt) and -Os.
VFS_SRCS = $(wildcard runtime/vfs/*.c)
VFS_TEST_SRCS = runtime/vm/test/test_main.c runtime/vm/test/test_host.c $(wildcard runtime/vfs/test/*.c)
VFS_INC = -Iruntime/vfs -Iruntime/vfs/test -Iruntime/vm -Iruntime/net -Iruntime/stream -Iruntime/vm/test -Iruntime/gpio -Iplatform/unix
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
INPUT_INC = -Iruntime/input -Iruntime/input/test -Iruntime/stream -Iruntime/vm -Iruntime/net -Iruntime/vm/test -Iruntime/gpio -Iplatform/unix
INPUT_DEPS = $(INPUT_SRCS) $(INPUT_TEST_SRCS) $(VM_SRCS) $(wildcard runtime/input/*.h) $(wildcard runtime/input/test/*.h) $(wildcard runtime/vm/*.h) $(PLAT_HDRS)

test-input: $(BUILD)/test_input $(BUILD)/test_input_mcu
	$(BUILD)/test_input
	$(BUILD)/test_input_mcu

$(BUILD)/test_input: $(INPUT_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(INPUT_INC) -o $@ $(INPUT_SRCS) $(INPUT_TEST_SRCS) $(VM_SRCS)

$(BUILD)/test_input_mcu: $(INPUT_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(MCU_FLAGS) -Os $(INPUT_INC) -o $@ $(INPUT_SRCS) $(INPUT_TEST_SRCS) $(VM_SRCS)

# stream (pure C99): Chunk delivery and Out bookkeeping against a fake "st"
# port on a real VM (and the port-send gate through the interpreter), in the
# host and MCU VM configs.
STREAM_TEST_SRCS = runtime/vm/test/test_main.c runtime/vm/test/test_host.c $(wildcard runtime/stream/test/*.c)
STREAM_INC = -Iruntime/stream -Iruntime/vm -Iruntime/net -Iruntime/vm/test -Iruntime/gpio -Iplatform/unix
STREAM_DEPS = $(STREAM_TEST_SRCS) $(VM_SRCS) $(wildcard runtime/stream/*.h) $(wildcard runtime/vm/*.h) $(wildcard runtime/vm/test/*.h) $(PLAT_HDRS)

test-stream: $(BUILD)/test_stream $(BUILD)/test_stream_mcu
	$(BUILD)/test_stream
	$(BUILD)/test_stream_mcu

$(BUILD)/test_stream: $(STREAM_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(STREAM_INC) -o $@ $(STREAM_TEST_SRCS) $(VM_SRCS)

$(BUILD)/test_stream_mcu: $(STREAM_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(MCU_FLAGS) -Os $(STREAM_INC) -o $@ $(STREAM_TEST_SRCS) $(VM_SRCS)

# screen (pure C99): the grid's operations, malformed and random sequences
# against guard words, the LCD renderer, and the "screen" port on a real VM
# (fake ScreenHw), in the host and MCU VM configs.
SCREEN_ALL_SRCS = $(wildcard runtime/screen/*.c) runtime/console/console.c runtime/console/font.c
SCREEN_TEST_SRCS = runtime/vm/test/test_main.c runtime/vm/test/test_host.c $(wildcard runtime/screen/test/*.c)
SCREEN_INC = -Iruntime/screen -Iruntime/console -Iruntime/stream -Iruntime/vm -Iruntime/net -Iruntime/vm/test -Iruntime/gpio -Iplatform/unix
SCREEN_DEPS = $(SCREEN_ALL_SRCS) $(SCREEN_TEST_SRCS) $(VM_SRCS) $(wildcard runtime/screen/*.h) $(wildcard runtime/console/*.h) $(wildcard runtime/vm/*.h) $(wildcard runtime/vm/test/*.h) $(PLAT_HDRS)

test-screen: $(BUILD)/test_screen $(BUILD)/test_screen_mcu
	$(BUILD)/test_screen
	$(BUILD)/test_screen_mcu

$(BUILD)/test_screen: $(SCREEN_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SCREEN_INC) -o $@ $(SCREEN_ALL_SRCS) $(SCREEN_TEST_SRCS) $(VM_SRCS)

# The same tests under AddressSanitizer and UBSan (build/asan-ub: its own
# flags, so it does not rebuild build/asan's).
test-screen-asan:
	$(MAKE) BUILD=build/asan-ub EXTRA_CFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -DGOGO_DEBUG=1" build/asan-ub/test_screen
	build/asan-ub/test_screen

$(BUILD)/test_screen_mcu: $(SCREEN_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(MCU_FLAGS) -Os $(SCREEN_INC) -o $@ $(SCREEN_ALL_SRCS) $(SCREEN_TEST_SRCS) $(VM_SRCS)

# The FatFs backend (runtime/vfs/fat) over a RAM disk, with the vendored
# FatFs (platform/esp32s3/components/fatfs) built for the host with the
# Cardputer's options (runtime/vfs/fat/test/stub); host slots are 192
# bytes (64-bit pointers in FIL).
FATFS_DIR = platform/esp32s3/components/fatfs/src
FAT_TEST_SRCS = runtime/vfs/fat/test/test_fat.c runtime/vm/test/test_main.c runtime/vfs/fat/fat_be.c runtime/vfs/vfs.c runtime/vfs/path.c
test-fat: $(BUILD)/test_fat
	$(BUILD)/test_fat

$(BUILD)/test_fat: $(FAT_TEST_SRCS) $(FATFS_DIR)/ff.c $(FATFS_DIR)/ffunicode.c $(wildcard runtime/vfs/fat/*.h) $(wildcard runtime/vfs/*.h) runtime/vm/test/test.h
	@mkdir -p $(BUILD)/fatfs
	$(CC) -std=gnu99 -O2 -g -w $(EXTRA_CFLAGS) -Iruntime/vfs/fat/test/stub -I$(FATFS_DIR) -c $(FATFS_DIR)/ff.c -o $(BUILD)/fatfs/ff.o
	$(CC) -std=gnu99 -O2 -g -w $(EXTRA_CFLAGS) -Iruntime/vfs/fat/test/stub -I$(FATFS_DIR) -c $(FATFS_DIR)/ffunicode.c -o $(BUILD)/fatfs/ffunicode.o
	$(CC) $(CFLAGS) -DGOGO_FS_SLOT_BYTES=192u -Iruntime/vfs/fat -Iruntime/vfs/fat/test/stub -I$(FATFS_DIR) -Iruntime/vfs -Iruntime/vm/test -Iruntime/vm -o $@ $(FAT_TEST_SRCS) $(BUILD)/fatfs/ff.o $(BUILD)/fatfs/ffunicode.o

# The LittleFS backend (runtime/vfs/lfs) over a RAM block device, with
# the LittleFS that the firmware build fetches (joltwallet/littlefs, a
# managed component; the repo does not carry it). Without it (no firmware
# built in this checkout yet) the test is skipped. Host slots are 192
# bytes (sizeof(Lf) with 64-bit pointers; 160 on the ESP32-S3).
LFS_DIR ?= platform/esp32s3/managed_components/joltwallet__littlefs/src/littlefs
ifneq ($(wildcard $(LFS_DIR)/lfs.c),)
LFS_TEST_SRCS = runtime/vfs/lfs/test/test_lfs.c runtime/vm/test/test_main.c runtime/vfs/lfs/lfs_be.c runtime/vfs/vfs.c runtime/vfs/path.c runtime/configsvc/configsvc.c
test-lfs: $(BUILD)/test_lfs
	$(BUILD)/test_lfs

$(BUILD)/test_lfs: $(LFS_TEST_SRCS) $(LFS_DIR)/lfs.c $(LFS_DIR)/lfs_util.c $(wildcard runtime/vfs/lfs/*.h) $(wildcard runtime/vfs/*.h) $(wildcard runtime/configsvc/*.h) runtime/vm/test/test.h
	@mkdir -p $(BUILD)/lfs
	$(CC) -std=c99 -O2 -g -w $(EXTRA_CFLAGS) -DLFS_NO_MALLOC -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR -Dmalloc=gogo_lfs_malloc -Dfree=gogo_lfs_free -I$(LFS_DIR) -c $(LFS_DIR)/lfs.c -o $(BUILD)/lfs/lfs.o
	$(CC) -std=c99 -O2 -g -w $(EXTRA_CFLAGS) -I$(LFS_DIR) -c $(LFS_DIR)/lfs_util.c -o $(BUILD)/lfs/lfs_util.o
	$(CC) $(CFLAGS) -DGOGO_FS_SLOT_BYTES=192u -Iruntime/vfs/lfs -I$(LFS_DIR) -Iruntime/vfs -Iruntime/configsvc -Iruntime/vm/test -Iruntime/vm -o $@ $(LFS_TEST_SRCS) $(BUILD)/lfs/lfs.o $(BUILD)/lfs/lfs_util.o
else
test-lfs:
	@echo "test-lfs: skipped (no LittleFS at $(LFS_DIR); a firmware build fetches it)"
endif

# The gpio driver (runtime/gpio) over a fake GpioHal on a real VM.
GPIO_TEST_SRCS = runtime/vm/test/test_main.c runtime/vm/test/test_host.c runtime/gpio/test/test_gpio.c
test-gpio: $(BUILD)/test_gpio
	$(BUILD)/test_gpio

$(BUILD)/test_gpio: $(GPIO_TEST_SRCS) $(VM_SRCS) $(wildcard runtime/vm/*.h) $(wildcard runtime/vm/test/*.h) $(PLAT_HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Iruntime/vm -Iruntime/net -Iruntime/stream -Iruntime/vm/test -Iruntime/gpio -Iplatform/unix -o $@ $(GPIO_TEST_SRCS) $(VM_SRCS)

# The gogo-config protocol (runtime/configsvc, pure C99) against an
# in-memory store and the memfs backend as /flash.
CONFIGSVC_SRCS = runtime/configsvc/configsvc.c runtime/vfs/path.c runtime/vfs/test/memfs.c
test-configsvc: $(BUILD)/test_configsvc
	$(BUILD)/test_configsvc

$(BUILD)/test_configsvc: $(CONFIGSVC_SRCS) runtime/configsvc/test/test_configsvc.c runtime/vm/test/test_main.c $(wildcard runtime/configsvc/*.h) $(wildcard runtime/vfs/*.h) $(wildcard runtime/vfs/test/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Iruntime/configsvc -Iruntime/vfs -Iruntime/vfs/test -Iruntime/vm -Iruntime/vm/test -o $@ runtime/configsvc/test/test_configsvc.c runtime/vm/test/test_main.c $(CONFIGSVC_SRCS)

# The host-directory backend (runtime/embed/fs_dir.c) against a temp dir:
# the vfs contract and the sandbox (symlinks, hard links, a swapped path).
test-fs-host: $(BUILD)/test_fs_host
	$(BUILD)/test_fs_host

$(BUILD)/test_fs_host: platform/unix/test/test_fs_host.c runtime/vm/test/test_main.c runtime/vm/test/test_host.c runtime/embed/fs_dir.c $(VFS_SRCS) $(VM_SRCS) $(wildcard runtime/vfs/*.h) $(PLAT_HDRS) $(wildcard runtime/vm/*.h) $(wildcard runtime/vm/test/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(VFS_INC) -Iruntime/embed -pthread -o $@ platform/unix/test/test_fs_host.c runtime/vm/test/test_main.c runtime/vm/test/test_host.c runtime/embed/fs_dir.c $(VFS_SRCS) $(VM_SRCS)

# Embed mode (runtime/embed, runtime/include/gogo.h): the API, the post
# inbox and the POSIX host over the VM core alone (no board), against images
# compiled from runtime/embed/test/testdata (and the EMBED_EXAMPLES, whose
# output is compared with their embed.out). test-embed-tsan runs the same
# tests under ThreadSanitizer; test-embed-leaks checks that no memory leaks
# (LeakSanitizer on Linux, leaks(1) on macOS, where ASan has no leak check).
EMBED_SRCS = $(wildcard runtime/embed/*.c)
EMBED_TEST_SRCS = runtime/vm/test/test_main.c $(wildcard runtime/embed/test/*.c)
EMBED_PROGS = $(notdir $(wildcard runtime/embed/test/testdata/*))
# Examples run under a test host, their output compared with embed.out
# (hostecho) or with their expected.out (the rest, by test_multi.c: 8 VMs
# on 4 threads; hotload's modules go into $(BUILD)/embed/hotload-flash).
EMBED_EXAMPLES = hostecho many_idle httpparse hotload
EMBED_INC = -Iruntime/include -Iruntime/embed -Iruntime/vfs -Iruntime/vm -Iruntime/board -Iruntime/vm/test

$(BUILD)/test_embed: $(EMBED_SRCS) $(EMBED_TEST_SRCS) $(VM_CORE_SRCS) $(FLOATLIB_SRCS) $(VFS_SRCS) $(wildcard runtime/embed/*.h) $(wildcard runtime/include/*.h) $(wildcard runtime/vm/*.h) $(wildcard runtime/vfs/*.h) $(wildcard runtime/vm/test/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DGOGO_EMBED $(EMBED_INC) -pthread -o $@ $(EMBED_TEST_SRCS) $(EMBED_SRCS) $(PARAMS_SRCS) $(VM_CORE_SRCS) $(FLOATLIB_SRCS) $(VFS_SRCS)

embed-images: gogo-cmd
	@mkdir -p $(BUILD)/embed
	for p in $(EMBED_PROGS); do $(BUILD)/gogo build runtime/embed/test/testdata/$$p -o $(BUILD)/embed/$$p.ggb || exit 1; done
	for p in $(EMBED_EXAMPLES); do $(BUILD)/gogo build examples/$$p -o $(BUILD)/embed/$$p.ggb || exit 1; done
	@mkdir -p $(BUILD)/embed/hotload-flash
	for m in worker_v1 worker_v2; do $(BUILD)/gogo build examples/hotload/mods/$$m -o $(BUILD)/embed/hotload-flash/$$m.ggb || exit 1; done

test-embed: $(BUILD)/test_embed embed-images
	GOGO_EMBED_IMAGES=$(BUILD)/embed $(BUILD)/test_embed

test-embed-tsan:
	$(MAKE) BUILD=build/tsan EXTRA_CFLAGS="-fsanitize=thread" build/tsan/test_embed embed-images
	TSAN_OPTIONS="halt_on_error=1" GOGO_EMBED_IMAGES=build/tsan/embed build/tsan/test_embed

ifeq ($(shell uname -s),Darwin)
test-embed-leaks: $(BUILD)/test_embed embed-images
	GOGO_EMBED_IMAGES=$(BUILD)/embed GOGO_EMBED_RACE_N=2000 leaks --atExit -- $(BUILD)/test_embed > $(BUILD)/embed/leaks.out 2>&1 || { cat $(BUILD)/embed/leaks.out; exit 1; }
	grep -E "passed|leaks for" $(BUILD)/embed/leaks.out
else
test-embed-leaks:
	$(MAKE) BUILD=build/asan EXTRA_CFLAGS="$(ASAN_FLAGS)" build/asan/test_embed embed-images
	ASAN_OPTIONS=detect_leaks=1 GOGO_EMBED_IMAGES=build/asan/embed GOGO_EMBED_RACE_N=2000 build/asan/test_embed
endif

# libgogo.a: the embedding library (runtime/include/gogo.h). The VM core,
# vfs, stream and the embed layer, built as strict C99 (-pedantic) with
# -DGOGO_EMBED; no board, input, net or tls. Link it with -pthread.
# The objects are prelinked into one relocatable object (ld -r) that keeps
# only the gogo_* API global (an export list on Darwin, objcopy on ELF), so
# the library's internal functions never clash with the host's;
# tools/libsyms.sh checks that its globals are exactly the list in
# runtime/include/libgogo.syms, which must also be gogo.h's functions.
# libgogo-debug.a is the same objects archived as they are: on macOS the
# prelinked object's debug info lives in build/lib/*.o (ld -r keeps a debug
# map, not the DWARF), so a library copied elsewhere for debugging is that
# one; its internal symbols are not hidden.
LIB_SRCS   = $(VM_CORE_SRCS) $(FLOATLIB_SRCS) $(VFS_SRCS) $(STREAM_SRCS) $(EMBED_SRCS) $(PARAMS_SRCS)
LIB_OBJS   = $(foreach f,$(LIB_SRCS),$(BUILD)/lib/$(subst /,_,$(patsubst runtime/%.c,%,$(f))).o)
LIB_CFLAGS = -std=c99 -pedantic -Wall -Wextra -Werror -O2 -g -fvisibility=hidden -DGOGO_EMBED -DGOGO_BUILD_UNIX_MS=$(BUILD_UNIX_MS)LL $(EXTRA_CFLAGS) \
             -Iruntime/include -Iruntime/embed -Iruntime/vm -Iruntime/vfs -Iruntime/stream -Iruntime/board
LIB_HDRS   = $(wildcard runtime/include/*.h) $(wildcard runtime/board/params.h) $(wildcard runtime/embed/*.h) $(wildcard runtime/vm/*.h) $(wildcard runtime/vfs/*.h) $(wildcard runtime/stream/*.h)
LIB_LD      ?= ld
LIB_OBJCOPY ?= objcopy

lib: $(BUILD)/libgogo.a $(BUILD)/libgogo-debug.a

$(BUILD)/libgogo.o: $(LIB_OBJS)
ifeq ($(shell uname -s),Darwin)
	@echo '_gogo_*' > $(BUILD)/lib/exports.txt
	$(LIB_LD) -r -exported_symbols_list $(BUILD)/lib/exports.txt -o $@ $(LIB_OBJS)
else
	$(LIB_LD) -r -o $@ $(LIB_OBJS)
	$(LIB_OBJCOPY) -w --keep-global-symbol='gogo_*' $@
endif

$(BUILD)/libgogo.a: $(BUILD)/libgogo.o runtime/include/gogo.h runtime/include/libgogo.syms
	@rm -f $@
	ar rcs $@ $(BUILD)/libgogo.o
	tools/libsyms.sh $@ runtime/include/libgogo.syms runtime/include/gogo.h runtime/include/gogo_posix.h || { rm -f $@; exit 1; }
	tools/globals.sh --objects $@ || { rm -f $@; exit 1; }

$(BUILD)/libgogo-debug.a: $(LIB_OBJS) runtime/include/libgogo.syms
	@rm -f $@
	ar rcs $@ $(LIB_OBJS)
	tools/libsyms.sh --subset $@ runtime/include/libgogo.syms || { rm -f $@; exit 1; }
	tools/globals.sh --objects $@ || { rm -f $@; exit 1; }

define LIB_OBJ_RULE
$(BUILD)/lib/$(subst /,_,$(patsubst runtime/%.c,%,$(1))).o: $(1) $(LIB_HDRS)
	@mkdir -p $(BUILD)/lib
	$(CC) $(LIB_CFLAGS) -c $(1) -o $$@
endef
$(foreach f,$(LIB_SRCS),$(eval $(call LIB_OBJ_RULE,$(f))))

# A host defining functions named like libgogo's internal ones links
# against libgogo.a and runs an image (the names must exist in the library,
# else the test proves nothing). It uses only the stable API: compiled with
# GOGO_EXPERIMENTAL as a deprecation, any experimental use would fail it.
CLASH_SYMS = msg_free heap_alloc vm_step bytes_new port_open

$(BUILD)/libgogo_clash: runtime/embed/test/clash/main.c $(wildcard runtime/include/*.h) $(BUILD)/libgogo.a
	$(CC) -std=c99 -pedantic -Wall -Wextra -Werror -O2 $(EXTRA_CFLAGS) -Iruntime/include \
		'-DGOGO_EXPERIMENTAL=__attribute__((deprecated("experimental")))' -o $@ $< $(BUILD)/libgogo.a -pthread

test-libgogo: $(BUILD)/libgogo_clash $(BUILD)/libgogo-debug.a gogo-cmd
	for s in $(CLASH_SYMS); do nm -g $(BUILD)/libgogo-debug.a | grep -Eq " T _?$$s$$" || { echo "test-libgogo: $$s is not in the library"; exit 1; }; done
	$(BUILD)/gogo build examples/many_idle -o $(BUILD)/libgogo_clash.ggb
	$(BUILD)/libgogo_clash $(BUILD)/libgogo_clash.ggb > $(BUILD)/libgogo_clash.out
	cmp $(BUILD)/libgogo_clash.out examples/many_idle/expected.out

# gogotools/embed, the Go binding over libgogo.a (build tag libgogo):
# vet, its tests, and the tests again under the race detector. cgo links
# $(BUILD)/libgogo.a (put ahead of the caller's CGO_LDFLAGS); go does not
# hash a C archive, so the archive's checksum goes into a link flag
# (GOBIND_FLAGS) to make go relink what it built against an older one.
GOBIND       = CGO_LDFLAGS="-L$(abspath $(BUILD)) $$CGO_LDFLAGS" go
GOBIND_FLAGS = -tags libgogo -ldflags="-X=gogotools/embed.libgogoSum=$$(cksum < $(BUILD)/libgogo.a | tr -d ' ')"

test-gobind: $(BUILD)/libgogo.a
	$(GOBIND) vet -tags libgogo ./embed/... ./examples/embed-go
	$(GOBIND) test $(GOBIND_FLAGS) -count=1 -timeout 120s ./embed/
	$(GOBIND) test $(GOBIND_FLAGS) -count=1 -timeout 120s -race ./embed/

# libgogoc.a: the compiler as a C library (embed/gogoc/gogoc.h), a Go
# c-archive prelinked so that only the gogoc_* functions are global
# (tools/libgogoc.sh, which checks them against embed/gogoc/libgogoc.syms).
# It carries the Go runtime. It is rebuilt when a Go source it is made of
# changes (the compiler, the gogo libraries it embeds, the embed package):
# go's archive differs at every build, so go cannot decide that alone. test-gogoc links it beside libgogo.a in a C program
# that compiles source and runs it, and that defines functions named like
# the Go runtime's C symbols.
libgogoc: $(BUILD)/libgogoc.a

GOGOC_SRCS = go.mod $(shell find compiler lib embed -name '*.go' -o -name '*.gg' -o -name '*.[ch]') tools/libgogoc.sh

$(BUILD)/libgogoc.a: $(GOGOC_SRCS)
	LIB_LD='$(LIB_LD)' LIB_OBJCOPY='$(LIB_OBJCOPY)' tools/libgogoc.sh $(BUILD)

$(BUILD)/gogoc/test_gogoc: embed/gogoc/test/test_gogoc.c embed/gogoc/gogoc.h $(wildcard runtime/include/*.h) $(BUILD)/libgogoc.a $(BUILD)/libgogo.a
	$(CC) -std=c99 -pedantic -Wall -Wextra -Werror -O2 -g $(EXTRA_CFLAGS) -Iembed/gogoc -Iruntime/include -o $@ $< $(BUILD)/libgogoc.a $(BUILD)/libgogo.a -pthread

test-gogoc: $(BUILD)/gogoc/test_gogoc
	$(BUILD)/gogoc/test_gogoc examples/many_idle

# examples/embed-c: host programs on libgogo.a, compile.c on libgogoc.a
# too; examples/embed-go: a Go host (examples/embed-c/test.sh runs them
# and compares their output).
EXAMPLE_EMBED_PROGS = main post port compile
EXAMPLE_EMBED_BINS  = $(addprefix $(BUILD)/embed-c/,$(EXAMPLE_EMBED_PROGS)) $(BUILD)/embed-go

$(BUILD)/embed-c/%: examples/embed-c/%.c $(wildcard runtime/include/*.h) $(BUILD)/libgogo.a
	@mkdir -p $(BUILD)/embed-c
	$(CC) -std=c99 -pedantic -Wall -Wextra -Werror -O2 -g $(EXTRA_CFLAGS) -Iruntime/include -o $@ $< $(BUILD)/libgogo.a -pthread

$(BUILD)/embed-c/compile: examples/embed-c/compile.c embed/gogoc/gogoc.h $(wildcard runtime/include/*.h) $(BUILD)/libgogoc.a $(BUILD)/libgogo.a
	@mkdir -p $(BUILD)/embed-c
	$(CC) -std=c99 -pedantic -Wall -Wextra -Werror -O2 -g $(EXTRA_CFLAGS) -Iembed/gogoc -Iruntime/include -o $@ $< $(BUILD)/libgogoc.a $(BUILD)/libgogo.a -pthread

$(BUILD)/embed-go: $(BUILD)/libgogo.a FORCE
	$(GOBIND) build $(GOBIND_FLAGS) -o $@ ./examples/embed-go

example-embed: $(EXAMPLE_EMBED_BINS) gogo-cmd
	examples/embed-c/test.sh $(BUILD)

# examples/dice-driver: a port driver in C (book chapter 22), compiled
# into a test-only board (board.c) on the unix host and nowhere else: not
# into gogovm or the firmware. test.sh runs its program there and compares
# the output with driver.out, and checks that gogovm refuses the image.
DICE_SRCS = examples/dice-driver/board.c examples/dice-driver/dice.c runtime/board/board_main.c $(PARAMS_SRCS) $(wildcard runtime/vfs/*.c) $(wildcard runtime/input/*.c)
$(BUILD)/dice-driver/dice-board: $(DICE_SRCS) $(VM_SRCS) $(wildcard runtime/vm/*.h) $(PLAT_HDRS) $(FLAGS_STAMP)
	@mkdir -p $(BUILD)/dice-driver
	$(CC) $(CFLAGS) -Iruntime/vm -Iruntime/net -Iruntime/vfs -Iruntime/input -Iruntime/stream -Iruntime/gpio -Iruntime/board -Iplatform/unix -o $@ $(DICE_SRCS) $(VM_SRCS)

example-driver: $(BUILD)/dice-driver/dice-board vm gogo-cmd
	examples/dice-driver/test.sh $(BUILD)

# Console text grid + font lookup (runtime/console), host build.
CONSOLE_SRCS = runtime/console/console.c runtime/console/font.c

test-console: $(BUILD)/test_console $(BUILD)/test_console_ascii
	$(BUILD)/test_console
	$(BUILD)/test_console_ascii

$(BUILD)/test_console: runtime/console/test/test_console.c runtime/vm/test/test_main.c runtime/vm/test/test.h $(CONSOLE_SRCS) $(wildcard runtime/console/*.h) runtime/console/font/gogo12.font
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Iruntime/console -Iruntime/vm/test -Iruntime/vm -o $@ runtime/console/test/test_console.c runtime/vm/test/test_main.c $(CONSOLE_SRCS)

# The same tests against the ASCII-only font (GOGO_FONT_CJK=n firmware).
$(BUILD)/test_console_ascii: runtime/console/test/test_console.c runtime/vm/test/test_main.c runtime/vm/test/test.h $(CONSOLE_SRCS) $(wildcard runtime/console/*.h) runtime/console/font/gogo12-ascii.font
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DFONT_ASCII -DFONT_PATH='"runtime/console/font/gogo12-ascii.font"' -Iruntime/console -Iruntime/vm/test -Iruntime/vm -o $@ runtime/console/test/test_console.c runtime/vm/test/test_main.c $(CONSOLE_SRCS)

# Render a text file through the console grid and font into a PPM:
#   make console-ppm TEXT=runtime/console/font/sample.txt OUT=build/sample.ppm
TEXT ?= runtime/console/font/sample.txt
OUT  ?= $(BUILD)/console.ppm

$(BUILD)/console_ppm: runtime/console/tools/console_ppm.c $(CONSOLE_SRCS) $(wildcard runtime/console/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Iruntime/console -o $@ runtime/console/tools/console_ppm.c $(CONSOLE_SRCS)

console-ppm: $(BUILD)/console_ppm
	$(BUILD)/console_ppm runtime/console/font/gogo12.font $(TEXT) $(OUT)

# docs/bytecode.md from the structured comments in runtime/vm/opcodes.h
# (tools/bcdoc); go test ./... (test-go) fails when it is stale.
bytecode-doc:
	go run ./tools/bcdoc

# runtime/vm/test/float_corpus.h: Go's FormatFloat / println output for a
# corpus of floats (tools/floatcorpus), which test_float.c checks gogo
# against; go test ./... (test-go) fails when it is stale.
float-corpus:
	go run ./tools/floatcorpus

test-go: vm $(BUILD)/gogovm_mcu $(BUILD)/gogovm_nofloat
	GOGO_VM=$(CURDIR)/$(BUILD)/gogovm GOGO_VM_MCU=$(CURDIR)/$(BUILD)/gogovm_mcu GOGO_VM_NOFLOAT=$(CURDIR)/$(BUILD)/gogovm_nofloat go test ./...

# gogovm without floating point (-DGOGO_FLOAT=0): the goldens marked
# `nofloat` run on it (an image that uses floats is refused at load).
$(BUILD)/gogovm_nofloat: $(VM_SRCS) $(MAIN_SRCS) $(wildcard runtime/vm/*.h) $(PLAT_HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DGOGO_FLOAT=0 -Iruntime/vm -Iruntime/net -Iruntime/vfs -Iruntime/input -Iruntime/stream -Iruntime/screen -Iruntime/console -Iruntime/embed -Iruntime/gpio -Iruntime/board -Iruntime/dist -Iplatform/unix -o $@ $(VM_SRCS) $(MAIN_SRCS)

# The standard library (lib/*/*.gg) on the host toolchain, copied into a
# temporary Go module gogo by tests/lib_test.go: vet (stdmethods off: gogo's
# bytes.Buffer.WriteByte deliberately returns no error) and its host tests
# (gogo/irproto, build tag gogohost). Part of test-go (go test ./...).
test-lib:
	go test -count=1 -run '^TestLibHost$$' ./tests/

# The network scenarios (dhcp, dns, ...) on a VM with the MCU actor memory
# limit (16 KB per actor): the library actors must fit.
$(BUILD)/gogovm_mcu: $(VM_SRCS) $(MAIN_SRCS) $(wildcard runtime/vm/*.h) $(PLAT_HDRS) $(TLS_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(MCU_FLAGS) -DGOGO_HOST_PORTS=16 -DGOGO_HOST_LINKS=8 $(TLS_CFLAGS) -Iruntime/vm -Iruntime/net -Iruntime/vfs -Iruntime/input -Iruntime/stream -Iruntime/screen -Iruntime/console -Iruntime/embed -Iruntime/gpio -Iruntime/board -Iruntime/dist -Iplatform/unix -o $@ $(VM_SRCS) $(MAIN_SRCS) $(TLS_LINK)

test-net-mcu: $(BUILD)/gogovm_mcu
	GOGO_VM=$(CURDIR)/$(BUILD)/gogovm_mcu go test -count=1 ./tests/net/

# The net and tls scenarios on a gogovm (and its mbedTLS) built with
# AddressSanitizer (build/asan). On macOS leaks are not reported (no
# LeakSanitizer); the tls tests check the TLS heap returns to 0 instead.
# GOGO_DEBUG=1 (runtime/vm/config.h): the sanitizer builds also check
# the runtime's internal invariants (GOGO_ASSERT).
ASAN_FLAGS = -fsanitize=address -fno-omit-frame-pointer -DGOGO_DEBUG=1
# ASAN_OPTIONS (the caller's, then these):
# - detect_leaks=0: LeakSanitizer (Linux only) would report the C suites'
#   fixtures and gogovm's exit, which do not free their Vm; test-embed-leaks
#   checks the embedding API for leaks.
# - detect_stack_use_after_return=0: with gcc's ASan it is on, and its fake
#   stack frames make mbedTLS's many small calls about 60 times slower (1 MB
#   over TLS: 0.18 s without, 10.6 s with, on Linux arm64), past the net
#   tests' 10 s limit. clang's ASan on macOS leaves it off too.
TLS_ASAN_OPTIONS = $${ASAN_OPTIONS:+$$ASAN_OPTIONS:}detect_leaks=0:detect_stack_use_after_return=0
test-tls-asan:
	$(MAKE) BUILD=build/asan EXTRA_CFLAGS="$(ASAN_FLAGS)" build/asan/gogovm build/asan/test_net build/asan/test_tls
	ASAN_OPTIONS="$(TLS_ASAN_OPTIONS)" build/asan/test_net
	ASAN_OPTIONS="$(TLS_ASAN_OPTIONS)" build/asan/test_tls
	ASAN_OPTIONS="$(TLS_ASAN_OPTIONS)" GOGO_VM=$(CURDIR)/build/asan/gogovm go test -count=1 ./tests/net/ ./tests/dist/
# Distributed actors under AddressSanitizer and UndefinedBehaviorSanitizer
# (build/san): tests/dist, its Go peer sending malformed and random frames
# ($GOGO_DIST_FUZZ of them) at the node port's codec.
SAN_FLAGS = -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -DGOGO_DEBUG=1
# runtime/dist's SHA-256 and HMAC-SHA-256 against known answers.
test-dist-c: $(BUILD)/test_hmac
	$(BUILD)/test_hmac

$(BUILD)/test_hmac: runtime/dist/test/test_hmac.c runtime/dist/hmac.c runtime/dist/hmac.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Iruntime/dist -o $@ runtime/dist/test/test_hmac.c runtime/dist/hmac.c

# A server at the caps' ceiling (2048 ports, 2047 links), filled with Go
# peers until its node agent refuses more (some 2000 file descriptors).
test-dist-scale: vm
	GOGO_DIST_SCALE=1 GOGO_VM=$(CURDIR)/$(BUILD)/gogovm go test -count=1 -run 'TestServerFill' -v ./tests/dist/

test-dist-san:
	$(MAKE) BUILD=build/san EXTRA_CFLAGS="$(SAN_FLAGS)" build/san/gogovm
	ASAN_OPTIONS="$(TLS_ASAN_OPTIONS)" GOGO_DIST_FUZZ=$${GOGO_DIST_FUZZ:-20000} GOGO_VM=$(CURDIR)/build/san/gogovm go test -count=1 ./tests/dist/

# Test code is host-only: it may use stdio and GCC/Clang constructors.
$(BUILD)/test_c: $(TEST_SRCS) $(VM_SRCS) $(wildcard runtime/vm/*.h) $(wildcard runtime/vm/test/*.h) $(PLAT_HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Iruntime/vm -Iruntime/net -Iruntime/stream -Iruntime/vm/test -Iruntime/gpio -Iruntime/board -Iplatform/unix -pthread -o $@ $(TEST_SRCS) $(VM_SRCS)

# Compile-check the core headers for a 32-bit target (no linking).
check32:
	for f in runtime/vm/check32.c $(VM_CORE_SRCS); do \
		clang --target=armv6m-none-eabi -ffreestanding -std=c99 -fsyntax-only -Wall -Wextra -Werror -Iruntime/vm -Iruntime/vm/test/stub $$f || exit 1; \
		clang --target=armv6m-none-eabi -ffreestanding -std=c99 -fsyntax-only -Wall -Wextra -Werror $(MCU_FLAGS) -Iruntime/vm -Iruntime/vm/test/stub $$f || exit 1; \
	done
	for f in $(FLOATLIB_SRCS); do \
		clang --target=armv6m-none-eabi -ffreestanding -std=c99 -fsyntax-only -Wall -Wextra -Werror -Iruntime/vm -Iruntime/vm/test/stub $$f || exit 1; \
	done
	for f in $(NET_SRCS); do \
		clang --target=armv6m-none-eabi -ffreestanding -std=c99 -fsyntax-only -Wall -Wextra -Werror -Iruntime/vm -Iruntime/net -Iruntime/stream -Iruntime/vm/test/stub $$f || exit 1; \
	done
	for f in $(DIST_SRCS); do \
		clang --target=armv6m-none-eabi -ffreestanding -std=c99 -fsyntax-only -Wall -Wextra -Werror -Iruntime/dist -Iruntime/stream -Iruntime/vm -Iruntime/vm/test/stub $$f || exit 1; \
	done
	for f in $(STREAM_SRCS); do \
		clang --target=armv6m-none-eabi -ffreestanding -std=c99 -fsyntax-only -Wall -Wextra -Werror -Iruntime/stream -Iruntime/vm -Iruntime/vm/test/stub $$f || exit 1; \
	done
	for f in $(VFS_SRCS); do \
		clang --target=armv6m-none-eabi -ffreestanding -std=c99 -fsyntax-only -Wall -Wextra -Werror -Iruntime/vfs -Iruntime/vm -Iruntime/vm/test/stub $$f || exit 1; \
	done
	for f in runtime/configsvc/*.c; do \
		clang --target=armv6m-none-eabi -ffreestanding -std=c99 -fsyntax-only -Wall -Wextra -Werror -Iruntime/configsvc -Iruntime/vfs -Iruntime/configsvc/test/stub $$f || exit 1; \
	done
	for f in runtime/gpio/*.c; do \
		clang --target=armv6m-none-eabi -ffreestanding -std=c99 -fsyntax-only -Wall -Wextra -Werror -Iruntime/gpio -Iruntime/vm -Iruntime/vm/test/stub $$f || exit 1; \
	done
	for f in runtime/screen/*.c; do \
		clang --target=armv6m-none-eabi -ffreestanding -std=c99 -fsyntax-only -Wall -Wextra -Werror -Iruntime/screen -Iruntime/console -Iruntime/stream -Iruntime/vm -Iruntime/vm/test/stub $$f || exit 1; \
	done
	for f in $(CONSOLE_SRCS); do \
		clang --target=armv6m-none-eabi -ffreestanding -std=c99 -fsyntax-only -Wall -Wextra -Werror -Iruntime/console -Iruntime/vm/test/stub $$f || exit 1; \
	done

# No mutable globals in runtime/ (embed mode): tools/globals.sh compiles it with the VM build's CC/CFLAGS.
globals: export GLOBALS_TLS_CFLAGS = $(TLS_CFLAGS)
globals:
	GLOBALS_CC='$(CC)' GLOBALS_CFLAGS='$(CFLAGS)' GLOBALS_MCU_FLAGS='$(MCU_FLAGS)' tools/globals.sh

# The guard must flag exactly the mutable symbols of its fixture (and no const ones).
# gcc on ELF names the function-local static ?.cnt (globals.sh), clang fx_fn.cnt.
globals-selftest:
	GLOBALS_CC='$(CC)' GLOBALS_CFLAGS='$(CFLAGS)' GLOBALS_MCU_FLAGS='$(MCU_FLAGS)' tools/globals.sh --print tools/selftest/globals_fixture.c | sed 's/ ?\.cnt$$/ fx_fn.cnt/' | LC_ALL=C sort | diff - tools/selftest/globals_fixture.expect

clean:
	rm -rf $(BUILD)

# VM core size on the ESP32-S3 as linked into the firmware (firmware flags, relaxed
# --gc-sections link from the VM's API); fails above 64 KB, skips without the toolchain.
vmsize:
	tools/vmsize.sh

# runtime/net/ code size (linked, as vmsize) and fixed state on the ESP32-S3, reported apart from the VM core.
netsize:
	tools/vmsize.sh --net

# runtime/net/tls/ plus the mbedTLS it links, on the ESP32-S3 (-Os), reported apart from the VM core.
tlssize:
	MBEDTLS_DIR=$(MBEDTLS_DIR) tools/vmsize.sh --tls

# runtime/dist/ (the "node" port) code size (linked, as vmsize) and fixed state on the ESP32-S3, reported apart from the VM core.
distsize:
	tools/vmsize.sh --dist

# runtime/vfs/ code size (linked, as vmsize) and fixed state on the ESP32-S3, reported apart from the VM core.
vfssize:
	tools/vmsize.sh --vfs

# runtime/screen/ code size (linked, as vmsize) and fixed state on the ESP32-S3, reported apart from the VM core.
screensize:
	tools/vmsize.sh --screen

# runtime/floatlib/ (printing and parsing floats, gogo/math) code size on the
# ESP32-S3 (linked, as vmsize), reported apart from the VM core; no fixed state allowed.
floatlibsize:
	tools/vmsize.sh --floatlib

# Everything built with CFLAGS / EXTRA_CFLAGS: rebuilt when they change.
STAMPED = $(addprefix $(BUILD)/,gogovm gogovm_mcu gogovm_nofloat test_c test_c_mcu test_net test_net_mcu test_tls \
          test_tls_threads test_vfs test_vfs_mcu test_input test_input_mcu test_stream test_stream_mcu \
          test_screen test_screen_mcu test_fs_host test_embed test_console test_console_ascii \
          console_ppm libgogo_clash) $(LIB_OBJS) $(MBEDTLS_OBJS)
$(STAMPED): $(FLAGS_STAMP)
