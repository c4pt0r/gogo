# gogo on the ESP32-S3 (M5StickS3, Cardputer ADV)

An ESP-IDF project whose `main` component is the ESP platform layer; it
compiles the VM core (`runtime/vm/*.c` except `check32.c`) in place from the repo
root, with the ESP values of the `runtime/vm/config.h` knobs:
`GOGO_DEFAULT_MAX_WORDS=4096`, `GOGO_TABLE_CAP=1024`, `GOGO_MAX_PORTS=16`,
`GOGO_EVQ_CAP=64`, `GOGO_ARENA_BYTES=163840` (160 KB), `-Os` (the board
configuration: 16-bit word counts, a 48-byte Actor, 64/16 Bytes handles,
64 KB of module code; `main.c` resolves the VM's parameters, the `vm.*`,
`node.max_links` and `code.mem` keys of NVS over these defaults, with
`vm_params_resolve` before each boot); and the
storage core (`runtime/vfs/*.c`) with `GOGO_FS_MAX_OPEN=4`,
`GOGO_FS_SLOT_BYTES=160` (one LittleFS file, see `/flash` below).

One source tree, two boards, chosen at build time with the CMake variable
`GOGO_BOARD` (`main/board.h` holds each board's pins and features):

| board | `GOGO_BOARD` | LCD (ST7789 240x135, SPI3) | power | storage |
|-------|--------------|----------------------------|-------|---------|
| M5StickS3 (ESP32-S3-PICO-1-N8R8) | `sticks3` (default) | MOSI 39, SCLK 40, DC 45, CS 41, RST 21, BL 38 | M5PM1 (`pm1.c`) | `/flash` |
| Cardputer ADV (M5Stamp S3A, ESP32-S3FN8, no PSRAM) | `cardputer` | MOSI 35, SCLK 36, DC 34, CS 37, RST 33, BL 38 | none | `/flash`, `/sd` (SPI2: CS 12, MOSI 14, CLK 40, MISO 39) |

The StickS3 build also takes `sdkconfig.sticks3` (PSRAM, see "PSRAM and
what the firmware provides" below); `CMakeLists.txt` adds it to
`SDKCONFIG_DEFAULTS` when none is given. After pulling this change, delete the
generated `platform/esp32s3/sdkconfig` (and `build/CMakeCache.txt`) once, so
the new defaults are applied: IDF keeps the values already in `sdkconfig`.

Both have 8 MB flash and the same partition table, so `gogo flash` writes
a program image to either without a board option.

## Toolchain

ESP-IDF **v6.1.1** (`idf.py --version` → `ESP-IDF v6.1.1`; GCC
esp-15.2.0_20251204, esptool v5.6.0), installed with

    mkdir -p ~/esp && cd ~/esp
    git clone -b v6.1.1 --recursive --shallow-submodules --depth 1 https://github.com/espressif/esp-idf.git
    cd esp-idf && ./install.sh esp32s3      # tools go to ~/.espressif

## Build and flash

From the repo root:

    . ~/esp/esp-idf/export.sh
    idf.py -C platform/esp32s3 build
    platform/esp32s3/vmsize.sh                         # VM code size gate (<= 65536)
    idf.py -C platform/esp32s3 -p /dev/cu.usbmodemXXXX flash   # firmware, once

The Cardputer ADV firmware builds in its own directory, with its own
sdkconfig (FatFs options from `sdkconfig.cardputer` on top of
`sdkconfig.defaults`; `SDKCONFIG` is relative to `platform/esp32s3`):

    idf.py -C platform/esp32s3 -B platform/esp32s3/build-cardputer -DGOGO_BOARD=cardputer \
        -DSDKCONFIG=build-cardputer/sdkconfig \
        -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.cardputer" build
    idf.py -C platform/esp32s3 -B platform/esp32s3/build-cardputer -p /dev/cu.usbmodemXXXX flash
    platform/esp32s3/vmsize.sh platform/esp32s3/build-cardputer

Check which board is on the port before flashing (`esptool chip-id`: the
StickS3 reports embedded PSRAM, the Cardputer ADV none).

Programs go to the separate `gogo` partition, so changing the program does
not rebuild or reflash the firmware:

    go build -o build/gogo ./cmd/gogo
    build/gogo build tests/golden/hello -o hello.ggb
    build/gogo flash hello.ggb              # [-port P] [-esptool path]
    build/gogo monitor -reset -timeout 5 -until "gogo: vm exited"

`gogo flash` needs `esptool` (or `esptool.py`) in `PATH` (export.sh puts it
there) or `-esptool`; `-port` defaults to the only `/dev/cu.usbmodem*`.
`gogo monitor` copies the console to stdout for `-t` seconds or until a
line containing `-until`; `-reset` resets the chip first (RTS pulse) so the
capture starts at boot. On this port DTR/RTS are the chip's reset and boot
strap, so `gogo monitor` sets the port up on its own descriptor (raw,
CLOCAL, no HUPCL, both lines idle in one ioctl) and never resets the chip
unless `-reset` is given. Only one program can use the serial port at a
time.

## Releases

`tools/release.sh [version]` builds the release assets into
`build/release/<version>/`: per board the default firmware's
`bootloader.bin`, `partition-table.bin`, `gogo.bin` and `flasher_args.json`
(named `<board>-<file>`, e.g. `cardputer-gogo.bin`), the `gogo` command for
darwin, linux and windows on amd64 and arm64, the license files that
must travel with these binaries (`LICENSE`, `THIRD_PARTY_NOTICES.md`,
`font-licenses.zip` with the console font's OFL texts, and
`LICENSE-APACHE-2.0.txt` for ESP-IDF, mbedTLS and the Wi-Fi libraries,
copied from the ESP-IDF tree), and `SHA256SUMS` over all of them. The
firmware builds are the plain ones above (Kconfig defaults, every feature
on); the IDF build directories go under `$GOGO_RELEASE_WORK` (default
`build/release-work`). `.github/workflows/release.yml` runs the same
steps on a `v*` tag in the `espressif/idf:v6.1.1` image and attaches the
assets to a **draft** GitHub release (a pre-release when the tag has a
`-`, e.g. `v1.0.0-rc1`). `gogo setup` downloads these assets by name (book
chapter 12) and writes only the files `flasher_args.json` lists, at its
offsets: never NVS (0x9000), never an erase, never a merged image.

A draft is invisible to `gogo setup` (and to `releases/latest`) until it
is published. Check it on both boards first, then publish:

    git tag v1.2.3 && git push origin v1.2.3     # the workflow drafts the release
    gh release download v1.2.3 -R c4pt0r/gogo -D /tmp/gogo-v1.2.3
    /tmp/gogo-v1.2.3/gogo-darwin-arm64 setup -yes -skip-wifi -release-url /tmp/gogo-v1.2.3   # chmod +x first; each board
    gh release edit v1.2.3 -R c4pt0r/gogo --draft=false

## PSRAM and what the firmware provides

**PSRAM (StickS3 only).** The ESP32-S3-PICO-1-N8R8 has 8 MB octal PSRAM
(`sdkconfig.sticks3`: `CONFIG_SPIRAM`, `SPIRAM_MODE_OCT`, 80 MHz,
`SPIRAM_IGNORE_NOTFOUND` so a board without it still boots). It is used for
two things: the code of hot-loaded modules (`gogo/code`) and, with
`GOGO_TLS`, mbedTLS's memory (see "TLS" below). Module code:
`esp_host`'s `code_alloc` / `code_free` in `main/platform_esp.c` are
`heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_32BIT)` / `heap_caps_free`.
`CONFIG_SPIRAM_USE_CAPS_ALLOC=y` (not malloc-wide) keeps `malloc` and every
internal-heap user on internal RAM: the VM arena (`esp_host.alloc` asks
for `MALLOC_CAP_INTERNAL`), Wi-Fi and the LCD buffers (`MALLOC_CAP_DMA` /
internal) are unchanged. Module code is only read as words by the
interpreter, so the slower external RAM costs nothing that matters. They live
in the `esp_host` table (`platform_esp.c`; the VM reaches them only through
it, there are no weak defaults). On the Cardputer (no PSRAM, `CONFIG_SPIRAM`
off) both hooks are `NULL`, and modules run from the received `Bytes` or a
copy in the VM's memory.

Cost, measured with `idf.py size` / `size-components`:
`gogo.bin` StickS3 883744 B with PSRAM (875200 B without), Cardputer 927424 B.
Static DIRAM (internal RAM, the part that comes out of the heap) is
123898 B with PSRAM against 118125 B without: **+5773 B**, about 1.7 % of the
340 KB, so the boot line's `free RAM` should drop by about that much on the
StickS3 (measured: 192052 B free at boot, against 197408 B without
PSRAM). It is IRAM code and data of the PSRAM init and
the cache/MMU support (`libesp_hw_support` +2.2 KB, `libesp_psram` +1.3 KB,
`libesp_driver_spi` +0.3 KB, `libesp_system` +0.2 KB, `libesp_mm` +0.1 KB):
the IDF puts it in IRAM because it runs while the cache is off. Trimmed:
`SPIRAM_MEMTEST=n` (no 8 MB boot test; it was in flash, so DIRAM is
unchanged, boot is faster). Not trimmable: `STDATOMIC_S32C1I_SPIRAM_WORKAROUND`
is derived from `SPIRAM`, not a setting. Not done: moving the heap
(`libheap`, 5.6 KB of IRAM with or without PSRAM) into flash with
`CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH`; it is the largest internal-RAM win
available, but it is unrelated to PSRAM, changes both boards, and is unsafe
where the heap is used while flash cache is disabled (flash writes on
`/flash`, Wi-Fi tasks), so it needs its own measurement. Why it is worth it:
module code (up to `code.mem`, 64 KB by default, and more if the user sets
it) then lives in 8 MB of otherwise unused memory instead of competing with
the VM arena and Wi-Fi for the 160 KB internal heap; one 64 KB module costs
more internal RAM on the Cardputer than the PSRAM support costs the StickS3.
`SPIRAM_IGNORE_NOTFOUND` makes a missing or failed PSRAM silent (modules then
go to the VM's memory); check the address `code_alloc` returns on
the first run.

**TLS (`GOGO_TLS`).** The "tls" port (`runtime/net/tls/tls.c`) is compiled against
ESP-IDF's own mbedTLS component (4.1.1, the same source the host build
uses with `runtime/net/tls/config`), configured in `sdkconfig.defaults` (restartable
ECC, TLS 1.2 client only, record buffers 16384 / 4096 fixed, X25519,
`HAVE_TIME_DATE`, the certificate bundle without cross-signed mode, `-O2`;
reasons there). The port's hooks are `esp_board.tls` (a `TlsHw`, `runtime/net/tls/tls.h`) in
`main/platform_esp.c`:
- every mbedTLS allocation (the port's and Wi-Fi's WPA crypto) goes
  through one allocator, installed first thing in `platform_esp_init`
  (`CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC`): PSRAM first on the StickS3 (a
  connection peaks at about 35 KB there; the internal heap does not move
  during a handshake), internal RAM on the Cardputer ADV. This is PSRAM's
  second use, beside module code;
- the wall clock (`gogo/sys`, `gogo/sntp`) is the system clock
  (`settimeofday`), which mbedTLS reads for certificate validity; unknown
  (0) until set after each boot;
- trust anchors: the bundle in flash; a chain is checked link by link
  (sliced) up to its last certificate, which is trusted if it is a bundle
  root itself (same subject and key, e.g. a cross-signed root; shared
  `runtime/net/tls/anchor.c`, host-tested) or else checked against its
  issuer's key in the bundle (IDF's callback). It relies on IDF v6.1.1
  internals (the bundle layout and symbols, `esp_crt_verify_callback`),
  listed in `platform_esp.c`;
- `lwip_inet_pton` (IPv4 only): mbedTLS's x509 calls `inet_pton`, which
  would otherwise link all of lwIP (whose `tcp_*` names clash with gogo's
  stack);
- the `TlsHw`'s `random` is `esp_fill_random` (IDF's mbedTLS draws its keys
  from the same source).

Measured on the StickS3 against example.com (`examples/httpsdemo`): a
handshake about 2.5 s (software ECC: chain 1.8 s, ServerKeyExchange 0.35
s), sliced steps 11-26 ms with a few near 50 ms, and one unsliced ECDHE
step of 204 ms (above the 50 ms target for one step, accepted as a
documented exception; see `docs/known-issues.md`).
`gogo.bin` with and without `GOGO_TLS`: StickS3 1090208 / 946464 B
(+143744: the bundle 66 KB, mbedTLS's TLS layer and X.509, and -O2 for
mbedTLS, about 28 KB), Cardputer ADV 1125376 / 982224 B (+143152); static
DIRAM +288 / +152 B. Without it neither tls.c nor mbedTLS's TLS layer is
linked, and the mbedTLS settings that would also change Wi-Fi's WPA crypto
go back to IDF's defaults (`sdkconfig.tls-off`): the TLS-off
firmware is within a few hundred bytes of the firmware before HTTPS
(StickS3 +224 B, Cardputer +112 B, DIRAM unchanged: the wall clock on the
system clock and the scheduler's due-now check).

A TLS-off build by hand adds the fragment (`gogo firmware -without tls`
adds it by itself):

    idf.py -C platform/esp32s3 -B platform/esp32s3/build-tlsoff \
        -DSDKCONFIG=build-tlsoff/sdkconfig \
        -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.sticks3;sdkconfig.tls-off" build

`gogo device stats` adds `psram_free`,
`psram_min`, `mbedtls_mem`, `mbedtls_mem_peak` and the `tls_*` counters
(`tls_max_step_us`: the longest slice; `tls_last_hs_us`: the last
handshake; `tls_last_error`: the last mbedTLS error).

**What the build provides.** `esp_board` (same file) lists only the port
drivers and mounts this build really has: `gpio` always; `netif`, `udp`,
`tcp` and `tcp_listen` (`GOGO_NET`); `tls` (`GOGO_TLS`, needs `GOGO_NET`;
see "TLS" below); `kbd` (Cardputer with `GOGO_KBD`); `power`
(`GOGO_BATTERY`); `imu` (`GOGO_IMU`); `ir_tx` (`GOGO_IR`); `ir_rx` (StickS3
with `GOGO_IR`); `mic` and `speaker` (`GOGO_AUDIO`); `screen` (`GOGO_SCREEN`, in builds with
the console task); the requirement `float` (`GOGO_FLOAT`, checked by the VM
itself, not a driver); the mounts `/flash` always
and `/sd` (Cardputer with `GOGO_SD`). An image or module that requires
another name is refused at boot (`gogo: image requires <name>, which this
VM lacks`) or by `code.Load` (`Unsupported`, `code.Missing` names it). `code.mem` (default
64 KB) limits the total module code.

## Flash layout (`partitions.csv`, 8 MB)

| name    | type/subtype | offset   | size     |
|---------|--------------|----------|----------|
| nvs     | data/nvs     | 0x9000   | 0x6000   |
| factory | app/factory  | 0x10000  | 0x200000 |
| gogo    | data/0x40    | 0x210000 | 0x100000 |
| gogofs  | data/0x83    | 0x310000 | 0x4F0000 |

The `gogo` partition holds a 16-byte little-endian header
`{0x4D494747 "GGIM", image_bytes, crc32_ieee(image), 0}` and the image
(format 15.x, `IMAGE_MAJOR`.`IMAGE_MINOR` in `runtime/vm/image.h`: the
firmware runs images of its major up to its own minor, and refuses others
with `gogo: unsupported image format M.m (this firmware runs 15.0 to
15.x)`). At boot the firmware checks the magic, the length (multiple of 4,
at most partition size - 16) and the CRC, then mmaps the partition and runs
the image in place. Console messages:

- `gogo: no image`: no header (blank partition, or not written by `gogo flash`)
- `gogo: bad image (<reason>)`: length, crc, or an `image_load` error
- `gogo <version>: image <N> bytes, arena <cap> bytes, free RAM <F> (largest <L>)`: boot line (internal RAM left after the console is up), then the program runs
- `gogo: vm exited (<code>), arena used <bytes>`: after `vm_run` returns

after which the firmware idles. The offset 0x210000 is also `partOffset`
in `cmd/gogo/flash.go`.

## Platform layer (`main/`)

- `platform_esp.c`: the board's `gogo_host` (`esp_host`) and `VmBoard`
  (`esp_board`: port drivers, among them the `gpio` and input driver
  instances over this board's hardware layers, the Wi-Fi netif, and the
  mounts `esp_flash_mount` / `esp_sd_mount`), see `runtime/vm/host.h` and
  design.md 5.11.1. Output to the USB-Serial/JTAG console
  (stdout line-buffered, flushed before each stderr write). Chunks from the
  internal-RAM heap, capped at `GOGO_ARENA_BYTES`. `now_ms` from
  `esp_timer_get_time()` (`platform_esp_ms`; the VM counts from its own
  start). `wait` blocks the VM task in
  `ulTaskNotifyTake(pdTRUE, timeout)`; `wake` (`platform_notify`) gives the task
  notification (`vTaskNotifyGiveFromISR` in an ISR), which latches: a notify
  before or during a wait ends it. The event ring's barrier is the VM's
  `vm_barrier()` (`__atomic_thread_fence`). `random` is `esp_random()` (the
  hardware generator; `sys.Random`, the `gogo/sntp` nonce). The wall clock
  (`sys.Time` / `sys.SetTime`) is the system clock (`settimeofday`), through
  `esp_host`'s `unix_time_ms` / `set_unix_time_ms`: unknown at every boot
  until `gogo/sntp` or `sys.SetTime` sets it, with the build time
  (`GOGO_BUILD_UNIX_MS`, from `main/CMakeLists.txt`) as the floor.
- `gpio_esp.c`: the `GpioHal` under the `gpio` port driver
  (`runtime/gpio/gpio.c`: cfg `{Pin, Mode, Edge, DebounceMs}`, default
  debounce 20 ms, `Write{Value}` on outputs, events `{pin, level}`, the
  same code as on unix). A watched input uses an any-edge interrupt; the
  handler reads the level and calls `gpio_edge`, which pushes to the event
  ring, then notifies, never locking or allocating. For a single-edge pin
  `gpio_edge` also drops an edge that comes less than the debounce time
  after the previous level change (a key release's bounce). The 49 pin
  slots (`GpioPin`, 20 B each) are this file's static storage. Refused
  pins (`board.h` `BOARD_RESERVED_MASK`): StickS3 19-20 (USB), 26-37 (in-package flash and octal
  PSRAM of the ESP32-S3-PICO-1-N8R8), 21, 38-41, 45 (LCD), 47-48 (M5PM1
  I2C), 42 and 46 (IR), 14-18 (codec I2S); Cardputer ADV 19-20 (USB), 26-32
  (flash), 33-38 (LCD), 12, 14, 39, 40 (SD card), 8-9 (I2C bus), 10 (battery
  ADC), 11 (keyboard INT), 44 (IR LED), 41-43 and 46 (codec I2S).
  The Cardputer's G0 button is free (Input, active low).
- `i2c_bus.c`: the board's one shared I2C master bus, created on first use
  (before the VM starts, or on the scheduler thread; never in an ISR):
  Cardputer ADV port 0, SDA G8, SCL G9 (TCA8418 keyboard, BMI270, ES8311); StickS3
  port 1, SDA G47, SCL G48 (M5PM1, BMI270, ES8311). Internal pull-ups on.
- `input_esp.c`: the `KbdHal` and `PowerHal` for `runtime/input/` (the `kbd`
  and `power` ports, design.md 5.4.3), and every input driver instance
  (`esp_kbd_driver`, `esp_power_driver`, `esp_imu_driver`, ...), each
  defined only when the build has the device; `esp_board` in
  `platform_esp.c` lists them.
  - Cardputer ADV keyboard: TCA8418 at 0x34, 400 kHz. Init: KP_GPIO1 (0x1D)
    = 0x7F, KP_GPIO2 (0x1E) = 0xFF, KP_GPIO3 (0x1F) = 0x00, CFG (0x01) |=
    0x03; flush KEY_EVENT_A (0x04) until 0, then INT_STAT (0x02) = 0x1F
    (every source: a latched GPI/overflow/lock bit would hold INT low). INT
    on G11 (falling edge): the ISR sets a flag and notifies the VM task;
    the scheduler thread reads KEY_EVENT_A (bit7 = press, low 7 bits = key
    number n, raw = n - 1) until it is empty, then writes INT_STAT = 0x1F
    and re-arms from the INT pin level (still low = an event arrived before
    the clear, keep the flag; no I2C re-check, so it cannot fail). A
    persistent I2C error backs off (10 ms doubling to 640 ms) instead of
    spinning. A 70-entry table maps n to a USB HID usage and its Fn variant.
  - Cardputer ADV battery: ADC1 on G10, 12 dB, curve-fitting calibration,
    mean of 8 samples, mV x 2 (divider). Charging and external power are
    unknown (-1): no signal is readable.
  - StickS3 battery (M5PM1, polled every `PeriodMs`): VBAT mV from
    0x22/0x23 (little-endian), external power = PWR_SRC (0x04) bit0, charging
    = PM1 GPIO0 level (GPIO_IN 0x12 bit0, low = charging). VBAT is the whole
    16-bit register (a full cell is above 4095 mV; no mask). On the first read
    the driver writes I2C_CFG (0x09) = 0 (idle sleep off; the console
    bring-up does this too, but not with `GOGO_CONSOLE=0`), sets GPIO_FUNC0
    (0x16) bits[1:0] to 00 (GPIO function) and switches GPIO0 to an input
    (GPIO_MODE 0x10 bit0 cleared), each only if it reads otherwise. No keyboard: opening `kbd` fails. There is no
    power-button event (the PM1 acts on clicks itself).
- `imu_esp.c` (`GOGO_IMU`, both boards): the `ImuHal` hooks for
  `runtime/input/imu.c` (design.md 5.4.6), a Bosch BMI270 on the shared I2C bus at
  400 kHz, probed at 0x69 then 0x68 (CHIP_ID 0x24; three rounds 10 ms
  apart). Both boards answer at 0x69 (bus scan on the Cardputer ADV: 0x18
  ES8311, 0x34 TCA8418, 0x69 BMI270). Polled only: neither
  board wires the IMU interrupt to an ESP32 GPIO (StickS3 INT1 goes to PM1
  GPIO4, the Cardputer's is not connected). The first open loads Bosch's
  legacy feature configuration (soft reset, `PWR_CONF` = 0, `INIT_CTRL` = 0,
  8192 B in 64-byte chunks to `INIT_DATA` 0x5E, each after its word address
  in `INIT_ADDR_0/1` 0x5B/0x5C, `INIT_CTRL` = 1, `INTERNAL_STATUS` & 0x0F ==
  1): about 250 ms of the scheduler thread, once per boot. Every open: both
  sensors off, legacy features enabled per gesture mask (feature pages via
  `FEAT_PAGE` 0x2F / 0x30: any/no-motion page 1, orientation 2, flat 3, step
  4, tap 5; the bits are in the file's header), `ACC_RANGE` 0x41 = 2 (+-8 g),
  `GYR_RANGE` 0x43 = 0 (+-2000 dps), `ACC_CONF`/`GYR_CONF` = 0xA0 | ODR,
  FIFO headerless gyro + accel (`FIFO_CONFIG_1` 0x49 = 0xC0; 12-byte frames
  gx gy gz ax ay az, already gogo's order), FIFO flushed, `INT_LATCH` = 1,
  features mapped to INT1 (output off) so `INT_STATUS_0` 0x1C latches them
  between polls; then `PWR_CTRL` 0x7D on (accel + gyro, or accel only for
  `RateHz` 0). With gestures the sensor runs at 100 Hz at least and
  `FIFO_DOWNS` 0x45 thins the FIFO to `RateHz`. `imu_read_fifo` reads
  `FIFO_LENGTH` and then whole frames (at most 960 B per call) from
  `FIFO_DATA` 0x26, dropping frames with a dummy half (`0x01|0x02 0x7F 0x00
  0x80`: the accelerometer has no sample yet, the first two frames after
  every open on both boards);
  `imu_read_gestures` reads `INT_STATUS_0` (tap kind from 0x20, step count
  and orientation from feature page 0). Axes are the chip's own (no board
  remap). On the StickS3 the open also makes sure PM1 rail L1 (`PWR_CFG`
  0x06 bit 2, LDO3V3, the BMI270's supply) is on; the PM1 turns it on at
  power-up anyway.
- `bmi270_legacy_config.c` (`GOGO_IMU`): the 8192-byte array
  `bmi270_legacy_config_file` from `bmi270_legacy.c` of
  github.com/boschsensortec/BMI270_SensorAPI, commit
  41129fcfe39c583ee5462d79195741945d51c1fe (2025-06-18), copied byte for
  byte under **Bosch's BSD-3-Clause license**, whose header is kept verbatim
  at the top of the file (redistributions in binary form must reproduce the
  notice: it is in the source and in this paragraph). It is flash data, not
  RAM. The legacy configuration was chosen over the base one (which
  M5Unified loads) because only it has tap, double/triple tap and
  orientation; it has no wrist gestures.
- `ir_esp.c` (`GOGO_IR`): the IR hooks on the RMT peripheral, 1 MHz
  resolution, one 48-symbol memory block per channel, no DMA.
  - TX (both boards: StickS3 G46, Cardputer G44): `ir_tx_open` (port open)
    creates the channel, the copy encoder and a 128-symbol buffer (512 B,
    internal heap), applies the carrier from `ir.TX` (`rmt_apply_carrier`,
    frequency and duty as given; checked: 38 kHz / 0.33 like M5's demo) and
    sets the pin to the strongest drive (`GPIO_DRIVE_CAP_3`; the default
    `CAP_2` is weaker, and the Cardputer's LED hangs on G44 through 22 ohms).
    `ir_tx_start` converts the timings into the buffer before it returns.
    `ir_tx_busy` is a flag cleared by the RMT done callback. `ir_tx_close`
    (port close) waits up to 100 ms for a running frame (NEC: 68 ms), then
    frees the channel, encoder and buffer.
  - RX (StickS3 G42 only): input inverted (the receiver module pulls low
    during a burst) with the internal pull-up on (its output is open
    collector: without it G42 floats low and nothing is received; the IDF
    6.1 RMT driver does not enable one), `signal_range_min_ns` 1000, `signal_range_max_ns` =
    `GapUs` x 1000 (`GapUs` at most 32767: the 15-bit idle threshold at
    1 MHz). Three frame slots of 129 symbols (1548 B, internal heap,
    allocated at open, freed at close); the RMT done callback (ISR) records
    the slot and re-arms `rmt_receive` on the next free one, so frames keep
    arriving between the 20 ms polls. A frame of 129 symbols is longer than
    256 values and is cut by `runtime/input/ir.c` (counted in `Dropped`). The frame's
    start time is the done time minus the gap and the frame's length.
  - `sdkconfig.defaults` moves the RMT ISRs and the encoder out of IRAM
    (`RMT_TX/RX_ISR_HANDLER_IN_IRAM=n`, `RMT_ENCODER_FUNC_IN_IRAM=n`): about
    3 KB of internal RAM; IR timings are hundreds of microseconds, so a cache
    miss does not matter.
  - **`/flash` writes and long frames.** The RMT interrupt is not cache-safe
    (`RMT_*_ISR_CACHE_SAFE` off; this does not depend on where the handler
    lives), so it is held off while a `/flash` write has the cache off. A TX
    frame longer than one 48-symbol block (about 47 symbols, 94 values) needs
    refills from that interrupt and can then be **garbled** (stale half-blocks
    are replayed); a long received frame can be cut. Frames that fit one block
    are unaffected, NEC (34 symbols) included; only their Done may come later.
  - StickS3 power (`pm1.c`): the IR LED and receiver are on EXT_5V, M5PM1
    `PWR_CFG` 0x06 bit 3 (the boost converter that also feeds the Grove and
    Hat 5 V pins, off after M5's init). It is reference-counted over the open
    IR ports (`ir_tx` and `ir_rx`): the first open turns it on (10 ms settle)
    if it was off, the last close turns it off again, only if gogo turned it
    on (checked on the board: `PWR_CFG` 0x17 -> 0x1F -> 0x17). **While it is
    on, the Grove and Hat 5 V pins are outputs: nothing may feed 5 V into
    them (M5 docs).** While `ir_rx` is open the speaker amplifier is held
    off: PM1 GPIO3 (`GPIO_OUT` 0x11 bit 3 first, then `GPIO_FUNC0` 0x16
    bit 3 = GPIO, `GPIO_DRV` 0x13 bit 3 = push-pull, `GPIO_MODE` 0x10 bit 3 =
    output, so it never glitches on). `pm1.c` drives it to "the speaker
    wants it and `ir_rx` does not block it" (`pm1_spk_amp`,
    `pm1_spk_amp_block`): a `speaker` open fails while `ir_rx` is open, and an
    `ir_rx` opened during playback silences the speaker until it closes. All PM1 users start with
    `pm1_wake` (poll the chip up to 200 ms, `I2C_CFG` 0x09 = 0: no idle
    sleep), once per boot.
  - Pins G42, G46 (StickS3) and G44 (Cardputer, also U0RXD; the console is
    USB-Serial/JTAG) are reserved: the gpio port refuses them.
- `audio_esp.c` (`GOGO_AUDIO`, both boards): the `MicHal` / `SpkHal` hooks
  for `runtime/input/audio.c`, on the ES8311 codec (I2C 0x18 on the
  shared bus, 100 kHz) and I2S. Pins (`board.h`, from M5Unified
  `_begin_audio`): StickS3 BCLK G17, WS G15, DOUT G14, DIN G16 (G18, the
  codec's MCLK, is not driven); Cardputer ADV BCLK G41, WS G43, DOUT G42,
  DIN G46 (no MCLK). All reserved for the gpio port. Details and sources at
  the top of the file; in short:
  - Half duplex: one simplex I2S channel (I2S_NUM_0, master, Philips,
    16-bit mono) created at open and deleted at close; every open restarts
    the codec (reg 0x00 = 0x80) and programs its direction.
  - The codec is clocked from BCLK (reg 0x01 bit 7), as M5Unified does: 16
    kHz uses 16-bit slots (BCLK 512 kHz, codec x8), 8 kHz 32-bit slots
    (BCLK 512 kHz, codec x4). Serial format written explicitly (0x09 =
    0x0A = 0x0C, I2S 16-bit).
  - mic: 11 DMA buffers of 30 ms (16 kHz: 960 B; 10.5 KB, internal RAM,
    allocated at open) so 300 ms wait in the IDF queue; reads take whole
    buffers through a staging copy; the first 2 buffers are discarded here
    (not in `runtime/input/audio.c`); a queue overflow is reported as lost bytes.
    `Gain` is dB above the platform's normal level (book 16.1); here
    `Gain` 0 = PGA 30 dB + ADC scale 24 dB (54 dB), and `Gain` adds digital
    volume. High-pass reg 0x1B = 0x0A, 0x1C = 0x64 (0x6A left a ~600 ms DC
    step after each open).
  - speaker: 4 DMA buffers of 10 ms refilled by the `on_sent` callback from an
    8 KB ring (internal RAM, allocated at open), so `spk_queued` is exact
    and the latency at most 40 ms. VolumePct 1..100 = the board's ceiling
    `AUDIO_SPK_MAX_DB` (`board.h`) minus 40 dB .. the ceiling, 0 mute. The
    ceiling is 0 dB on the Cardputer ADV and **-4 dB on the StickS3**: at DAC
    0 dB its USB console once stopped answering (probably a supply dip from
    the amplifier; `docs/known-issues.md`).
  - Amplifier: StickS3: rail, codec, I2S running silence, then the
    amplifier (PM1 GPIO3; a failure to switch it fails the open); close:
    mute, amplifier off. Cardputer ADV: no amplifier enable pin is known
    (M5Unified's ADV callback drives none): taken to be always on.
  - The codec's analog reference stays on after the first open: powered
    down, every open began with about 1 s of exact zeros (ES8311 warm-up).
    The first `mic` open after power-up still has it. The idle current of
    the reference is not measured (`docs/known-issues.md`).
  - `GOGO_AUDIO` selects `CONFIG_I2S_ISR_IRAM_SAFE` (the two callbacks are
    `IRAM_ATTR`): without it a `/flash` write holds the I2S interrupt off and
    the GDMA reports only the last finished buffer, so the others would be
    lost (mic) or replayed (spk). Recording 16 kHz to `/flash` loses nothing
    (`examples/audiohw`).
  - Checks: `examples/audiohw` (half duplex, record and analyse, tone
    timing, playback), `examples/audiotone` + `examples/audiolisten` in
    `platform/esp32s3/examples` (one board plays, the other listens).
  - Off (`GOGO_AUDIO=n`): `audio_esp.c` and the IDF I2S driver are not
    linked; `runtime/input/audio.c` (the common driver) still is, its ports fail to
    open. Cost of audio (2026-11-25): StickS3 +18720 B firmware, +592 B
    static DIRAM; Cardputer ADV +19680 B, +820 B.
- `image_part.c`: the partition check and mmap above.
- `main.c`: the VM runs in a FreeRTOS task pinned to CPU1. It brings the board up
  (console, config service, Wi-Fi, `/flash` and `/sd`), maps the image and
  boots it through `gogo_board_main` (`runtime/board`, the boot path the
  unix VM uses too), with the `Vm`, `VfsState`, `InputState` and
  `NetState` in static storage.
- Screen (debug console, mirror of stdout/stderr):
  - `pm1.c` (StickS3 only): M5PM1 (I2C G47/G48, 0x6E): I2C idle sleep and watchdog off,
    PM1 GPIO2 high = LCD rail, 200 ms settle (M5GFX/M5Unified StickS3
    sequence, cited in the file). Also `pm1_wake` (idle sleep off, once, for
    every PM1 user), EXT_5V (`PWR_CFG` bit 3) and rail L1 (bit 2) on, and the
    speaker amplifier (GPIO3) for `ir_esp.c` / `audio_esp.c`, and the codec
    rail (L3B, GPIO2, the LCD's) for `audio_esp.c`.
  - `lcd_st7789.c`: ST7789 135x240 used as landscape 240x135 (column
    offset 40, row offset 53, MADCTL 0x6C, inverted; the same on both
    boards), SPI3 40 MHz with DMA, pins from `board.h`; backlight G38.
  - `console_task.c`: `runtime/console` console grid + embedded
    `runtime/console/font/gogo12.font`. The VM task appends to the grid
    under a short spinlock and notifies the render task (CPU0, priority 1),
    which snapshots the grid and redraws only dirty rows, at most once per
    50 ms, one 240x12 band per SPI transfer. stdout white, stderr (crash
    reports) red, platform lines (boot, exit, image errors) white. If the
    font blob is invalid the console draws box glyphs (logged once on
    serial); if the PM1 or the LCD fails the console stays off (logged).
    `linker.lf` places the renderer in internal RAM (out of the flash
    cache) and `sdkconfig.defaults` enlarges the caches (32 KB instruction,
    64 KB data, 64-byte lines), because the VM (CPU1) and the render task
    (CPU0) share them and rendering from flash evicted the VM's code.

- `fs_lfs.c`: `/flash`, see below.
- `wifi.c`: Wi-Fi station and `runtime/net/netif.h` (M5). The driver's raw frames go
  straight to gogo's own stack (`runtime/net/`, compiled into the firmware) through
  the driver's internal interface, pinned to ESP-IDF v6.1.1:
  `esp_wifi_internal_reg_rxcb(WIFI_IF_STA, rx_cb)` (the callback runs in the
  Wi-Fi task on CPU0, offers the frame to the stack's single RX slot with
  `net_rx_offer(&esp_net_state, ...)`, which copies or drops it, and frees
  the driver buffer at once), `esp_wifi_internal_tx` (copies; called from the VM task) and
  `esp_wifi_get_mac`. `esp_netif` and lwIP are never initialised and none of
  their code is linked. The station connects with `wifi.ssid` /
  `wifi.password` from the config store (WPA2 at least when a password is
  set, strongest AP of that name, no modem sleep), reconnects after every
  disconnect (or refused connect call) with backoff 1 s doubling to 32 s
  (logged on serial: `gogo: wifi retry in <ms> ms`), and reconnects at
  once when `gogo config` changes a `wifi.` key. All station state (backoff,
  retry timer) is changed only on the default event-loop task: the retry
  timer, `gogo config` and the scan post gogo's own events to it. Link changes go to `net_link_event`.
  Screen lines: `gogo: wifi not configured (gogo config set wifi.ssid ...)`,
  `gogo: wifi connected (<ssid>, ch <n>, <rssi> dBm)`, `gogo: wifi lost
  (<why>, reason <n>), reconnecting`, `gogo: wifi: <why> (reason <n>),
  retrying` (first failure only), `gogo: ip <a.b.c.d> (gateway <g>)` when
  the stack goes up (`wifi_esp_netif.report`) and `gogo: network down`.
- `config_esp.c`: the config store (NVS namespace `gogo`, one blob per key;
  `sys.Config` reads it, `wifi.` keys hidden) as the `KvHal` of the
  `gogo config` service (`runtime/configsvc`, which implements the
  protocol for every board), and the board's side of it: a task (CPU0)
  reads the USB-Serial/JTAG input (driver installed, stdio switched to it)
  and feeds it to the service, which answers frames
  `\x02gogo-config <op> [<key> [<value>]]\n` with `\x02kv <text>` lines and
  then `\x02ok[ <text>]` or `\x02err <reason>`; ops `set get rm list`,
  `push data end abort` (`gogo push`, into `/flash` through its backend,
  which takes the volume's lock) and `format-flash`, plus the board's
  `scan` (nearby networks) and `stats` (net counters, `heap_free`,
  `heap_min`, `arena_used`, and the fixed tables in use as
  `bytes=N/64`, `ports=N/16`, `subs=N/4`; the capacities are the Vm's,
  `vm.handles` and `vm.max_ports`), and `info`, one line
  `\x02ok fw=<version> board=<sticks3|cardputer> image=<major>.<minor>
  actor_memory=16K memory=160K max_actors=1024 max_ports=16 max_links=8
  code_mem=64K bytes_handles=64/16 provides=<name>,...`
  (`GOGO_FIRMWARE_VERSION`, `BOARD_NAME`, the image format, the VM's
  parameters as `main.c` resolved them at boot (`vm_params_resolve`: the
  config keys over the build's defaults; `board_params_format`, values
  shown here are the defaults), and `esp_board`'s drivers and mounts plus
  `float`: what an image's requirements are checked against), which `gogo device info`
  prints and `gogo setup` compares with the firmware it is about to
  flash; a firmware older than `info` answers `\x02err bad key`. The
  version is the `git describe` of the repo at configure time (`dev`
  without git), set in `CMakeLists.txt` as the app's `PROJECT_VER`, so
  it is also in `gogo.bin`'s app descriptor, where setup reads it. The password is never sent back (`(set)`).
  Program output never carries `\x02` on serial (written as `?`), so a
  program cannot fake a reply; buffers that held a frame are zeroed after
  each command, and `get wifi.password` reads only the value's length.
  Serial output does not block the VM when no host reads the port: IDF's
  driver-mode stdio tries a full TX ring once for 50 ms and then drops
  (checked: netdemo kept fetching with the port closed).
  The NVS partition is not written by `idf.py flash` or `gogo flash`, so
  settings survive reflashing.

Wi-Fi setup (once per board; `gogo config` talks to the board when exactly
one is connected and `$GOGO_CONFIG` is unset, or with `-port`; with none
or several connected it stops with an error):

    build/gogo device scan                      # which networks the board sees (2.4 GHz only)
    build/gogo config set wifi.ssid MyNetwork
    build/gogo config set wifi.password -       # reads the password from stdin
    build/gogo config list

Wi-Fi memory (measured on the M5StickS3), internal RAM. The buffer
settings in `sdkconfig.defaults` are the smallest *tested* working ones,
not proven minima: 2 static RX (the Kconfig minimum) + 2 dynamic RX
buffers, 2 dynamic TX buffers, 1 RX management buffer (minimum), 6
management short buffers (minimum), no AMPDU, no SoftAP/enterprise,
driver NVS off, IRAM speed options off. With them DHCP, DNS, HTTP GET, 20
sequential and 3 x 4 parallel page loads and 20 pings all pass; 0 dynamic
RX would mean "unlimited" in IDF, so 1 was not tried below 2.

| what | bytes |
|---|---|
| static (.data/.bss/IRAM) added by Wi-Fi, PHY, supplicant, NVS, USB driver | about 31.5 K |
| heap: `esp_wifi_init` (incl. default event loop) | 25.1 K (25.7 K with 4/4/2 buffers) |
| heap: `esp_wifi_start` | 1.8 K |
| heap: associated (WPA2), steady state | about 2.4 K |
| heap: dynamic RX/TX buffers under traffic (at most 2 + 2 frames) | up to about 6.5 K, transient |
| gogo net stack fixed state (`esp_net_state`, a `NetState` in main.c, the tls port's state included) | 3704 |
| config handler (scratch, line buffer; task stack 4 K) | about 2.1 K + 4 K |
| free internal heap at the boot line (before the VM; was 275 K without Wi-Fi) | 197 K |
| free internal heap running `examples/netdemo` (`gogo device stats`) | about 173 K (arena 22 K in use) |

The boot logs the driver's heap use on serial (`gogo: wifi heap: ...`).

The receive path drops frames while the single RX slot is full
(`rx_busy`); TCP's one-segment windows keep that rare.

## `/flash` (LittleFS)

`/flash` is LittleFS through the portable backend
`runtime/vfs/lfs/lfs_be.c` (the `vfs` backend, the mount policy below);
`fs_lfs.c` is this board's part: the `gogofs` partition as a `BlockDev`,
the volume's static storage and the `fs.flash.*` settings. LittleFS is from the
`joltwallet/littlefs` component (pinned `==1.23.3` in
`main/idf_component.yml`, bundling littlefs v2.11), used only through its
raw `lfs.h` API (`CONFIG_LITTLEFS_EXPOSE_RAW_API=y`), never its ESP-IDF VFS
wrapper, on the `gogofs` partition. Neither `idf.py flash` nor `gogo flash`
writes that partition, so files survive reflashing (the whole-flash erase
`idf.py erase-flash` wipes them, and the Wi-Fi settings).

- Config: `read_size 16`, `prog_size 16`, `block_size 4096`,
  `cache_size 64`, `lookahead_size 16`, `block_cycles 500`. No malloc: the
  read / prog caches and lookahead buffer are static and each open file's
  64-byte cache is in its vfs slot (`lfs_file_opencfg`). Enforced at build
  time (`platform/esp32s3/CMakeLists.txt`): `lfs.c` alone is compiled with
  `LFS_NO_MALLOC` and with `malloc` / `free` renamed to `lfs_be.c`'s
  NULL-returning stubs, so its object imports no `malloc` / `free`. (The
  component's `CONFIG_LITTLEFS_MALLOC_STRATEGY_DISABLE` cannot be used: its
  unused VFS wrapper does not compile with it.) `CONFIG_LITTLEFS_ASSERTS=n`:
  an internal assert on a corrupt partition would abort at every boot.
- Fixed state (logged at boot as `gogo: fs state N bytes`, at most 3072,
  checked at compile time in `fs_lfs.c`): 1112 bytes = vfs tables 708 (4
  slots of 168: an `lfs_file_t`, its `lfs_file_config` and cache, plus slot
  header; the mount table) + LittleFS 404 (the `LfsVol`: `lfs_t`,
  `lfs_config`, two 64-byte caches, the 16-byte lookahead, the block
  device and options pointers and the mounted flag; the 36-byte `BlockDev`). An open file charges its owner
  160 bytes.
- Size: config `fs.flash.size` (bytes, `K` or `M` suffix, rounded down to
  4 KB, clamped to 64 KB .. the partition; unset = the whole partition,
  5056 KB; a bad value is said on the console and means the whole
  partition). At boot the size is read from the superblock; a larger
  setting grows the file system in place (`lfs_fs_grow`, files kept); a
  smaller one keeps the current size and prints `gogo: /flash shrink needs
  format (gogo device format-flash)`. `gogo device format-flash` formats it:
  the board answers, sets a flag in RTC memory and restarts, and the
  next mount sees the flag (only after that software restart; it is
  cleared at once) and formats at the set size (`gogo: formatting /flash
  (gogo device format-flash)`; everything on `/flash` is lost). NVS is not
  written. The deprecated key `fs.flash.format` = `yes` still formats
  once at the next boot (`gogo: formatting /flash (fs.flash.format=yes)`)
  and is removed; the key is removed before formatting, and if that fails
  nothing is formatted (`gogo: could not clear fs.flash.format; /flash not
  formatted (gogo config rm fs.flash.format)`), so a stuck key cannot wipe
  `/flash` at every boot. A blank or unreadable partition (mount says corrupt or
  invalid; first boot) is formatted: `gogo: formatting /flash` (LittleFS
  logs `Corrupted dir pair at {0x0, 0x1}` first). "Invalid" also covers a
  file system from a newer LittleFS on-disk version or with a larger name
  limit, so a firmware downgrade can wipe `/flash` (PLAT-5, deferred). Any other mount error
  (flash I/O), or a file system bigger than the partition, leaves `/flash`
  unmounted (every path `NotMounted`) with its data untouched and a console
  line. Serial only: `gogo: /flash <N> KB (partition 5056 KB)`.

      build/gogo config set fs.flash.size 1M
      build/gogo device format-flash              # only to shrink, or to wipe
      build/gogo config rm fs.flash.size          # back to the whole partition

- Measured on the M5StickS3: filling the whole partition with 4 KB writes
  runs at about 68 KB/s (5 MB in 75 s) and ends in `NoSpace`; removing the
  file returns `Usage` to its earlier value. A fresh file system uses
  24 KB (root, `kv/` and the demo's log directory included).

## `/sd` (FAT, Cardputer ADV)

`/sd` is FatFs through the portable backend `runtime/vfs/fat/fat_be.c`
(the `vfs` backend, the lazy mount); `fs_fat.c` is this board's part on
boards with an SD slot: the card as a `DiskHal`, FatFs's disk layer
hooked to it, the volume's static storage. FatFs
(ESP-IDF's `fatfs` component, its `f_*` API only, never `esp_vfs_fat_*`;
FAT12/16/32 and exFAT, from the vendored copy in `components/fatfs` with
`FF_FS_EXFAT 1`, see its README)
over the `sdspi` driver on SPI2 (SPI3 is the LCD's), 20 MHz, retried once
at 400 kHz. Details are in the file's top comment; in short:

- Lazy mount, no reboot needed: boot only registers `/sd`. Every path call
  checks the card (one CMD13 when mounted) and, without a mounted card,
  initialises and mounts it. Without a card every call is `NotMounted`
  quickly; a card inserted later works on the next call; a disk error
  unmounts, so the next call remounts. A card with neither FAT nor exFAT
  is `NotMounted` (`gogo: /sd: no FAT or exFAT file system on the card
  (<what is there>; ...)` once); the firmware never formats a card.
- FatFs config (`sdkconfig.cardputer`): no per-file cache (`FF_FS_TINY`),
  512-byte sectors (`CONFIG_WL_SECTOR_SIZE_512`: FatFs takes the larger of
  the SD and wear-levelling sector sizes), window inside the static
  `FATFS` (`CONFIG_FATFS_USE_DYN_BUFFERS=n`), long names on the stack (max
  64), code page 437, no FatFs lock table, one volume. FatFs's mutexes
  (hard-coded reentrant in IDF, heap-allocated) are link-time wrapped to
  no-ops (`main/CMakeLists.txt`): only the VM task calls in.
- No malloc per file or per transfer: `FATFS`, the card state and a
  512-byte bounce sector (for buffers the SPI DMA cannot take directly) are
  static; a `FIL` lives in its vfs slot. The IDF drivers allocate once on
  the first call (SPI bus, sdspi device, a 20-byte diskio entry), and card
  initialisation uses short-lived buffers.
- Names compare ignoring case (FatFs matches short and long names that way,
  and the vfs slot hash is lower-cased). `Rename` cannot replace, so
  `WriteFile` is `.tmp`, remove, rename (not atomic). A `Write` open
  (truncate) of a path open for reading is `Busy` on `/sd` (vfs rule for
  case-insensitive backends: FatFs would free the clusters under the
  reader). `Usage` is clamped to 2 GB - 1 on bigger cards, and its first call on an
  exFAT card (or FAT32 without FSINFO) scans the allocation map.
- Fixed state (Cardputer build, logged as `gogo: fs state N bytes`, checked
  at compile time in `fs_fat.c`): 2424 bytes = vfs 708 + LittleFS 404 +
  `/sd` 1312 (the `FatVol` 588: `FATFS` 572 with its 512-byte window and
  the volume's fields; `sdmmc_card_t` 204, the 512-byte bounce sector). A `FIL` is 80 bytes (slot 160).

Firmware cost of storage (vfs + B_FS + LittleFS + `fs_lfs.c`): `gogo.bin`
844272 -> 866880 bytes (+22.1 KB, LittleFS asserts off); free internal
heap at the boot line 197672 -> 196736 (-936, measured with asserts on;
the fixed state is in `.bss`).

Firmware build switches (Kconfig, `main/Kconfig.projbuild`; `idf.py menuconfig`
under "gogo", or `CONFIG_...=n` lines in an `SDKCONFIG_DEFAULTS` overlay):

- `GOGO_KBD` (default y): the Cardputer's TCA8418 keyboard. Off: `port.Open("kbd")`
  fails with `errors.NotFound`, `input_esp.c` has no keyboard code, and `i2c_bus.c` is not
  built on the Cardputer, so the IDF I2C master driver drops out of the link.
  No effect on the StickS3 (no keyboard).
- `GOGO_BATTERY` (default y): the battery reader (Cardputer ADC, StickS3 M5PM1).
  Off: `port.Open("power")` fails with `errors.NotFound` and no ADC or PM1 battery code is
  linked. The StickS3 still uses I2C for the PM1 LCD rail when the console is on.

- `GOGO_IMU` (default y): the BMI270 driver (`imu_esp.c`) and its 8 KB
  configuration. Off: `port.Open("imu")` fails with `errors.NotFound`, there is no `imu` driver, and
  on the Cardputer with `GOGO_KBD` also off `i2c_bus.c` and the IDF I2C master
  driver drop out.
- `GOGO_IR` (default y): `ir_esp.c` on the RMT. Off: `port.Open` of `ir_tx`
  and `ir_rx` fails with `errors.NotFound` (the drivers are not listed), and the IDF RMT
  driver is not linked (checked: no `rmt_new_tx_channel` in `gogo.elf`). The
  IR pins stay reserved.

  `gogo.bin` / static DIRAM (ESP-IDF 6.1.1, fresh build directories; before
  input phase 2 the defaults were StickS3 885712 / 123978 and Cardputer
  929408 / 121084):

  | variant | StickS3 `.bin` | DIRAM | Cardputer `.bin` | DIRAM |
  |---|---|---|---|---|
  | default (IMU and IR on) | 923472 | 124882 | 957888 | 121664 |
  | `GOGO_IMU=n` | 912992 | 124866 | 947472 | 121648 |
  | `GOGO_IR=n` | 897680 | 123978 | 941248 | 121084 |
  | both off | 887232 | 123978 | 930864 | 121084 |

  The IMU costs about 10.5 KB of flash (8 KB configuration, about 2 KB of
  code) and 16 B of DIRAM (6 B of state plus alignment); IR costs 16.6 KB (Cardputer, TX only) to
  25.8 KB (StickS3) of flash, mostly the IDF RMT driver and the GDMA code it
  references, and 580 / 904 B of DIRAM (the RMT and GDMA ISR glue that stays
  in IRAM, plus `ir_esp.c`'s 85 B of state). With both off the firmware is
  still about 1.5 KB above the earlier default: the chip-independent
  `runtime/input/imu.c`, `runtime/input/ir.c` and `runtime/stream/` are always linked (the ports
  exist and refuse to open). Run time, all in internal RAM and only while
  open: the IMU batch buffer (`va_alloc`, up to 2400 B, charged to the
  owner) and the `ir_rx` frame buffer (512 B, `va_alloc`, charged) come from
  the VM arena; the RMT buffers come from the system heap (TX 512 B from the
  port open, plus the IDF channel objects; RX 1548 B while `ir_rx` is
  open). The RMT buffers and channel objects are **not charged** to the
  owning actor (only the arena buffers above are); they are system heap,
  held only while the port is open.

- `GOGO_AUDIO` (default y): `audio_esp.c`, the `mic` and `speaker` ports on the
  ES8311 and I2S (both boards; see `audio_esp.c` above). It selects
  `CONFIG_I2S_ISR_IRAM_SAFE`. Off: `port.Open` of `mic` and `speaker` fails
  with `errors.NotFound` (the drivers are not listed), and the IDF I2S driver is not linked;
  `runtime/input/audio.c` (the common driver) is still linked. Cost: StickS3 +18720 B
  `gogo.bin`, +592 B static DIRAM; Cardputer ADV +19680 B, +820 B.
- `GOGO_SCREEN` (default y): the `screen` port (`runtime/screen/*.c`, its
  hooks in `console_task.c`): a 40 x 11 cell grid drawn by the console's
  render task while the port is open. Off: `runtime/screen` is not built,
  `port.Open("screen")` fails with `errors.NotFound` and an image that requires
  `screen` is refused. The driver is also left out of a `GOGO_CONSOLE=0`
  build, and its open fails when the LCD failed to start. Cost
  (ESP-IDF 6.1.1, against the same tree without it): StickS3 1093056 ->
  1098224 B `gogo.bin` (+5168), Cardputer ADV 1128352 -> 1133440 (+5088);
  a 1760-byte grid is allocated from internal RAM at the first open and
  kept.
- `GOGO_FLOAT` (default y): floating point, `float32` and `float64`
  (`runtime/vm/floats.c` built with `GOGO_FLOAT=1`, and
  `runtime/floatlib/` for printing floats, `gogo/math` and
  `strconv.FormatFloat` / `ParseFloat`, exact and without the C library's
  conversions). Off: the VM lacks the requirement `float`, so an image
  that uses floats is refused at boot (`gogo: image requires float, which
  this VM lacks`) and `code.Load` returns `Unsupported`. `float32`
  arithmetic uses the chip's single-precision FPU, `float64` libgcc's
  software routines (a `float64` operation costs about twice a `float32`
  one; book 3.2). Cost (ESP-IDF 6.1.1, the same tree with
  `CONFIG_GOGO_FLOAT=n`): StickS3 1101952 -> 1111552 B `gogo.bin`
  (+9600), Cardputer ADV 1137072 -> 1146800 (+9728), +80 B static
  DIRAM. Of that, the VM core's share is about 1.8 KB (`tools/vmsize.sh`
  against `--no-float`), `runtime/floatlib` about 7.9 KB (`make
  floatlibsize`), and the rest the soft-float routines they link. The
  board's C library (picolibc) `strtod` / `strtof` are not linked:
  ParseFloat has its own exact parser.
- `GOGO_NET` (default y): Wi-Fi and gogo's net stack. Off: `wifi.c` and `runtime/net/*.c`
  are not built, the drivers `netif`, `udp`, `tcp` and `tcp_listen` are not
  listed (an image that requires one is refused at boot, naming it), `port.Open` of `netif`, `udp`,
  `tcp` and `tcp_listen` fails with `errors.NotFound`, `net.Subscribe` returns false (no port
  source), and `gogo device scan` answers "no network in this firmware" and `gogo device stats` prints only the memory figures.
  `gogo config set wifi.*` still stores the value. `esp_wifi`, `wpa_supplicant`,
  `libnet80211`, `libpp` and `libphy` are not linked.
  `gogo.bin` (ESP-IDF 6.1.1): StickS3 883744 B default, 460368 B `GOGO_NET=n`;
  Cardputer 927424 B default, 506784 B `GOGO_NET=n`. Static DIRAM drops by
  about 33 KB (StickS3 123898 -> 91063 B, Cardputer 121004 -> 88501 B).

- `GOGO_SD` (default y; no effect on the StickS3): `/sd` on FatFs. Off:
  `fs_fat.c` is not built, FatFs, `sdmmc` and `sdspi` are not linked, `/sd` is
  not registered (every path is `NotMounted`, as with no card) and not listed
  in the board's mounts (an image that requires `/sd` is refused at boot). The SD pins (G12, G14, G39,
  G40) stay reserved and the gpio port still refuses them: the slot is wired to
  them and a card may be in it, so a program driving them could corrupt it;
  this is the same choice as the keyboard INT and battery pins with `GOGO_KBD`
  and `GOGO_BATTERY` off.
- `GOGO_FS_EXFAT` (default y, needs `GOGO_SD`): exFAT. `components/fatfs/src/
  ffconf.h` derives `FF_FS_EXFAT` from it. Off: only FAT12/16/32 mount; an exFAT
  card takes the existing "no FAT or exFAT file system on the card" path
  (`NotMounted`).
- `GOGO_FONT_CJK` (default y): the console font. Off: `gogo12-ascii.font` (95
  printable ASCII glyphs, 1536 B, generated by `go run ./cmd/gogo-font -ascii
  -o runtime/console/font/gogo12-ascii.font <bdf>`, see
  `runtime/console/font/README.md`) is embedded instead of `gogo12.font`
  (162958 B); every other character, Chinese included, draws as the console's
  box in the same one or two columns. `make test-console` runs the console
  tests against both fonts.

  `gogo.bin` (ESP-IDF 6.1.1) and static DIRAM, Cardputer: default 927424 B /
  121004 B; `GOGO_SD=n` 884800 B / 118320 B (-42.6 KB flash, -2.7 KB RAM);
  `GOGO_FS_EXFAT=n` 923536 B / 121004 B (-3.9 KB flash); `GOGO_FONT_CJK=n`
  766000 B / 121004 B (-161.4 KB flash, the font is flash data); `GOGO_NET=n
  GOGO_SD=n GOGO_FONT_CJK=n` 303152 B / 85817 B. StickS3 `GOGO_FONT_CJK=n`:
  883744 B -> 722320 B.

### Global slimming (measured, not applied)

Each row is one extra `SDKCONFIG_DEFAULTS` overlay line on the default build
(ESP-IDF 6.1.1, fresh build directory, the generated `sdkconfig` checked for
the option). `.bin` is `gogo.bin` in bytes, DIRAM the static internal RAM from
`idf.py size`; the difference is against the default row. No default was
changed: these are choices for a firmware someone builds for one purpose.

| overlay | StickS3 `.bin` | DIRAM | Cardputer `.bin` | DIRAM |
|---------|----------------|-------|------------------|-------|
| default | 883728 | 123898 | 927424 | 121004 |
| (a) `LOG_DEFAULT_LEVEL_WARN=y` | 883728 (0) | 123898 (0) | 927408 (-16) | 121004 (0) |
| (b) `LOG_DEFAULT_LEVEL_ERROR=y` | 873712 (-10016) | 123228 (-670) | 917168 (-10256) | 120579 (-425) |
| (c) `COMPILER_OPTIMIZATION_CHECKS_SILENT=y` | 864016 (-19712) | 122235 (-1663) | 902240 (-25184) | 118933 (-2071) |
| (d) `ESP_WIFI_ENABLE_WPA3_SAE=n` | 834320 (-49408) | 123674 (-224) | 878064 (-49360) | 120764 (-240) |
| (e) b + c + d | 804592 (-79136) | 121328 (-2570) | 842480 (-84944) | 118247 (-2757) |

(a) is the default already: `sdkconfig.defaults` leaves `LOG_DEFAULT_LEVEL` at
the IDF's WARN (`CONFIG_LOG_DEFAULT_LEVEL=2` in the default build's
`sdkconfig`), so it is a no-op and only (b) lowers it. The default row of the
StickS3 is 16 B below the number quoted earlier in this file and the Cardputer
row of (a) 16 B below its default: the `.bin` embeds the build directory path,
so ignore differences of that size.

What each one costs, in plain words:

- Log level ERROR (b): the IDF and Wi-Fi stop printing warnings (for example
  the Wi-Fi retry and association messages); only errors reach the serial
  port. gogo's own console and `gogo:` boot lines do not use the IDF log and
  are unchanged. Fewer diagnostics when something goes wrong in the IDF.
- `COMPILER_OPTIMIZATION_CHECKS_SILENT` (c): `ESP_ERROR_CHECK` and the IDF's
  assert macros still stop the chip, but without the failing expression, file
  and line text; a crash then says less about where it came from.
- WPA3 off (d): the supplicant is built without SAE, so the board cannot join
  an access point that is WPA3-only (WPA2 and WPA2/WPA3 mixed networks still
  work). It matters only with `GOGO_NET=y`; with `GOGO_NET=n` there is
  nothing to remove. This is the largest single item.
- Together (e) the flash savings are the sum of the three (StickS3 -79136 B
  both ways; Cardputer -84944 B measured against -84800 B summed).

**Warning:** a changed Kconfig default or an `SDKCONFIG_DEFAULTS` overlay does
not apply to a build directory that already has an `sdkconfig`: the existing
values win. Use a fresh `-B` directory, change the value with `idf.py
menuconfig`, or delete that build directory's `sdkconfig` first. (`gogo
firmware` does this for its own directory, see below.)

### `gogo firmware`: features from what the programs need

`gogo firmware -board sticks3|cardputer [-o dir] [-build] [-with list]
[-without list] files...` reads the `requires` word of the given images and
modules and writes `<dir>/sdkconfig.gogo` (`CONFIG_GOGO_NET`, `_KBD`,
`_BATTERY`, `_SD`, `_FS_EXFAT`, `_FONT_CJK`, `_IMU`, `_IR`, `_AUDIO`, `_TLS`,
`_SCREEN`, `_FLOAT`), then prints (or, with
`-build`, runs) the matching build:

    idf.py -C platform/esp32s3 -B <dir> -DGOGO_BOARD=<board> -DSDKCONFIG=<dir>/sdkconfig \
        -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;<board overlay>;<dir>/sdkconfig.gogo" build

Feature names are `net kbd power sd exfat cjk imu irtx irrx mic speaker tls screen float`; `net`, `kbd`,
`power` and `float` are on only if required or given with `-with`; `sd`, `exfat`, `cjk`,
`imu`, `irtx`, `irrx`, `mic`, `speaker`, `tls` are on by default and `-without` turns them off
(`irtx` and `irrx` are one switch, `GOGO_IR`; `mic` and `speaker` are one, `GOGO_AUDIO`; `tls` needs `net`, `GOGO_TLS` depends on `GOGO_NET`). A required feature cannot be removed,
and one the board lacks (`kbd`, `sd` on the StickS3) is an error. A feature
that is off may add its own sdkconfig fragment to `SDKCONFIG_DEFAULTS` (the
table's `offDefaults`): `tls` off adds `sdkconfig.tls-off`, which puts
mbedTLS back to IDF's defaults for Wi-Fi's WPA crypto. The board
feature table is in `cmd/gogo/firmware.go` and must match `main/board.h` and
`main/Kconfig.projbuild`. `gogo firmware` owns its build directory: if
`<dir>/sdkconfig` exists and any `CONFIG_GOGO_*` value in it differs from the
new overlay, the tool deletes it (and says so) so the new switches take
effect; if nothing differs it is kept, so incremental builds stay fast.

With `KBD` and `BATTERY` both off the Cardputer has no input hardware; disabled
behaves like absent hardware. Example, own build directory per variant:

    printf 'CONFIG_GOGO_KBD=n\nCONFIG_GOGO_BATTERY=n\n' > /tmp/noinput.defaults
    idf.py -C platform/esp32s3 -B platform/esp32s3/build-cp-off -DGOGO_BOARD=cardputer \
        -DSDKCONFIG=build-cp-off/sdkconfig \
        -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.cardputer;/tmp/noinput.defaults" build

`gogo.bin` sizes (ESP-IDF 6.1.1): Cardputer default 922416 B, Cardputer
KBD and BATTERY off 897200 B (all optional features off: 276160 B), StickS3 default 870288 B, StickS3 `GOGO_BATTERY=n` 870080 B.

FatFs timestamps are a fixed time (2025-01-01 00:00:00): `fs_fat.c` wraps
`get_fattime`, because IDF's calls `localtime_r` and drags picolibc's `tzset`,
`sscanf` and `vfscanf` (7.7 KB of flash) into the Cardputer image.

Console build switches (`idf.py -D...`, kept in each build directory's
CMake cache, so use a separate `-B` directory per variant):

    idf.py -C platform/esp32s3 -B platform/esp32s3/build-noconsole -DGOGO_CONSOLE=0 build
    idf.py -C platform/esp32s3 -B platform/esp32s3/build-verbose -DGOGO_CONSOLE_VERBOSE=1 build

`GOGO_CONSOLE=0` compiles the console out (timing baseline);
`GOGO_CONSOLE_VERBOSE=1` logs bring-up time, frame stats and the VM's time
in console writes on serial.

Examples:

- `examples/keys`: on-device gpio check: prints KEY1 (G11) edges and KEY2
  (G12) presses.
- `examples/console`: mixed Chinese/English lines and one forced actor
  crash (red on screen).
- `examples/printbench`: 1000 `println` lines, then the elapsed ms.
- `examples/memfill`: actors grow by recursion until one hits oom; the exit line shows the arena really filled.
- `examples/fsdemo` (repo root): `/flash` demo and device tests: boot
  counter in `kv`, KEY1/KEY2 presses appended to a `ringlog`, `Usage` on
  screen and serial; `gogo config set fsdemo.mode keep|fill|powercut`
  selects a test (see the file's comment).
- `examples/sddemo` (repo root, Cardputer ADV): `/sd` device checks
  (see the file's comment).
- `examples/netdemo` (repo root): DHCP, a Chinese/English page on port 80
  (KEY1/KEY2 counts, last fetch, uptime) and an `http.Get` of `demo.url`
  every 10 s.

netdemo usage (after the Wi-Fi setup above; the password is never written
to a file or the command line, only typed to `gogo config set wifi.password -`):

    build/gogo config set demo.url http://example.com/   # optional fetch target
    build/gogo build examples/netdemo -o netdemo.ggb
    build/gogo flash netdemo.ggb

The screen shows `gogo: wifi connected ...`, then `gogo: ip <addr> (gateway
<g>)` and the page URL; open `http://<addr>/` in a browser on the same LAN.
The page (Chinese/English) shows the KEY1/KEY2 counts, fetches ok/total and
uptime, and refreshes every 5 s; the device also prints an `http.Get` result
every 10 s. `demo.url` may be an `https://` URL (see "TLS" below and
`examples/httpsdemo`).

- `examples/httpsdemo` (repo root): HTTPS on the board. DHCP, `sntp.Sync`
  (pool.ntp.org), one `http.Get("https://example.com/")`, three requests
  through an `http.NewClient` (one handshake), and a "tls" port to
  example.com's address with the wrong `Host`, which must be refused; then
  it idles. Build and flash it as netdemo above; `gogo device stats` then
  shows the TLS counters.
