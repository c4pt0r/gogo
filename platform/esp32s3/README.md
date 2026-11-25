# gogo on the ESP32-S3 (M5StickS3, Cardputer ADV)

An ESP-IDF project whose `main` component is the ESP platform layer; it
compiles the VM core (`vm/*.c` except `check32.c`) in place from the repo
root, with the ESP values of the `vm/config.h` knobs:
`GOGO_DEFAULT_MAX_WORDS=4096`, `GOGO_TABLE_CAP=1024`, `GOGO_MAX_PORTS=16`,
`GOGO_EVQ_CAP=64`, `GOGO_ARENA_BYTES=163840` (160 KB), `-Os`; and the
storage core (`vfs/*.c`) with `GOGO_FS_MAX_OPEN=4`,
`GOGO_FS_SLOT_BYTES=160` (one LittleFS file, see `/flash` below).

One source tree, two boards, chosen at build time with the CMake variable
`GOGO_BOARD` (`main/board.h` holds each board's pins and features):

| board | `GOGO_BOARD` | LCD (ST7789 240x135, SPI3) | power | storage |
|-------|--------------|----------------------------|-------|---------|
| M5StickS3 (ESP32-S3-PICO-1-N8R8) | `sticks3` (default) | MOSI 39, SCLK 40, DC 45, CS 41, RST 21, BL 38 | M5PM1 (`pm1.c`) | `/flash` |
| Cardputer ADV (M5Stamp S3A, ESP32-S3FN8, no PSRAM) | `cardputer` | MOSI 35, SCLK 36, DC 34, CS 37, RST 33, BL 38 | none | `/flash`, `/sd` (SPI2: CS 12, MOSI 14, CLK 40, MISO 39) |

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
    build/gogo build tests/golden/hello -o hello.gogo
    build/gogo flash hello.gogo              # [-port P] [-esptool path]
    build/gogo monitor -reset -t 5 -until "gogo: vm exited"

`gogo flash` needs `esptool` (or `esptool.py`) in `PATH` (export.sh puts it
there) or `-esptool`; `-port` defaults to the only `/dev/cu.usbmodem*`.
`gogo monitor` copies the console to stdout for `-t` seconds or until a
line containing `-until`; `-reset` resets the chip first (RTS pulse) so the
capture starts at boot. On this port DTR/RTS are the chip's reset and boot
strap, so `gogo monitor` sets the port up on its own descriptor (raw,
CLOCAL, no HUPCL, both lines idle in one ioctl) and never resets the chip
unless `-reset` is given. Only one program can use the serial port at a
time.

## Flash layout (`partitions.csv`, 8 MB)

| name    | type/subtype | offset   | size     |
|---------|--------------|----------|----------|
| nvs     | data/nvs     | 0x9000   | 0x6000   |
| factory | app/factory  | 0x10000  | 0x200000 |
| gogo    | data/0x40    | 0x210000 | 0x100000 |
| gogofs  | data/0x83    | 0x310000 | 0x4F0000 |

The `gogo` partition holds a 16-byte little-endian header
`{0x4D494747 "GGIM", image_bytes, crc32_ieee(image), 0}` and the image
(format v8, lockstep with `IMAGE_VERSION` in `vm/image.h`; when the
image version changes, reflash the firmware too: an older firmware refuses
a newer image with `gogo: bad image (unsupported version)`, and a newer firmware an older
one). At boot the firmware checks the magic, the length (multiple of 4,
at most partition size - 16) and the CRC, then mmaps the partition and runs
the image in place. Console messages:

- `gogo: no image`: no header (blank partition, or not written by `gogo flash`)
- `gogo: bad image (<reason>)`: length, crc, or an `image_load` error
- `gogo <version>: image <N> bytes, arena <cap> bytes, free RAM <F> (largest <L>)`: boot line (internal RAM left after the console is up), then the program runs
- `gogo: vm exited (<code>), arena used <bytes>`: after `vm_run` returns

after which the firmware idles. The offset 0x210000 is also `partOffset`
in `cmd/gogo/flash.go`.

## Platform layer (`main/`)

- `platform_esp.c`: `vm/platform.h`. Output to the USB-Serial/JTAG console
  (stdout line-buffered, flushed before each stderr write). Chunks from the
  internal-RAM heap, capped at `GOGO_ARENA_BYTES`. `platform_now_ms` from
  `esp_timer_get_time()`. `platform_wait` blocks the VM task in
  `ulTaskNotifyTake(pdTRUE, timeout)`; `platform_notify` gives the task
  notification (`vTaskNotifyGiveFromISR` in an ISR), which latches: a notify
  before or during a wait ends it. `platform_barrier` is
  `__sync_synchronize()`.
- `gpio_esp.c`: the `gpio` port driver, same semantics as unix (cfg
  `{Pin, Mode, Edge, DebounceMs}`, default debounce 20 ms, `Write{Value}` on
  outputs, events `{pin, level}`). Inputs use an any-edge interrupt; the ISR
  pushes to the event ring and notifies, never locking or allocating. For a
  single-edge pin it also drops an edge that comes less than the debounce
  time after the previous level change (a key release's bounce). Refused
  pins (`board.h`): StickS3 19-20 (USB), 26-37 (in-package flash and octal
  PSRAM of the ESP32-S3-PICO-1-N8R8), 21, 38-41, 45 (LCD), 47-48 (M5PM1
  I2C); Cardputer ADV 19-20 (USB), 26-32 (flash), 33-38 (LCD), 12, 14,
  39, 40 (SD card), 8-9 (I2C bus), 10 (battery ADC), 11 (keyboard INT).
  The Cardputer's G0 button is free (Input, active low).
- `i2c_bus.c`: the board's one shared I2C master bus, created on first use
  (before the VM starts, or on the scheduler thread; never in an ISR):
  Cardputer ADV port 0, SDA G8, SCL G9 (TCA8418 keyboard); StickS3 port 1,
  SDA G47, SCL G48 (M5PM1). Internal pull-ups on.
- `input_esp.c`: the `InputHw` for `input/` (the `kbd` and `power` ports,
  design.md 5.4.3); `platform_input` itself is defined in `platform_esp.c`
  so the strong definition always beats the weak default in `input/source.c`.
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
- `image_part.c`: the partition check and mmap above.
- `main.c`: the VM runs in a FreeRTOS task pinned to CPU1.
- Screen (debug console, mirror of stdout/stderr):
  - `pm1.c` (StickS3 only): M5PM1 (I2C G47/G48, 0x6E): I2C idle sleep and watchdog off,
    PM1 GPIO2 high = LCD rail, 200 ms settle (M5GFX/M5Unified StickS3
    sequence, cited in the file).
  - `lcd_st7789.c`: ST7789 135x240 used as landscape 240x135 (column
    offset 40, row offset 53, MADCTL 0x6C, inverted; the same on both
    boards), SPI3 40 MHz with DMA, pins from `board.h`; backlight G38.
  - `console_task.c`: `platform/common` console grid + embedded
    `platform/common/font/gogo12.font`. The VM task appends to the grid
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
- `wifi.c`: Wi-Fi station and `net/netif.h` (M5). The driver's raw frames go
  straight to gogo's own stack (`net/`, compiled into the firmware) through
  the driver's internal interface, pinned to ESP-IDF v6.1.1:
  `esp_wifi_internal_reg_rxcb(WIFI_IF_STA, rx_cb)` (the callback runs in the
  Wi-Fi task on CPU0, offers the frame to the stack's single RX slot with
  `net_rx_offer`, which copies or drops it, and frees the driver buffer at
  once), `esp_wifi_internal_tx` (copies; called from the VM task) and
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
  the stack goes up (`platform_net_report`) and `gogo: network down`.
- `config_esp.c`: the config store (NVS namespace `gogo`, one blob per key;
  `config.Get` reads it, `wifi.` keys hidden) and the `gogo config` command
  handler: a task (CPU0) reads the USB-Serial/JTAG input (driver installed,
  stdio switched to it) and answers frames
  `\x02gogo-config <op> [<key> [<value>]]\n` with `\x02kv <text>` lines and
  then `\x02ok[ <text>]` or `\x02err <reason>`; ops `set get rm list`
  plus `scan` (nearby networks) and `stats` (net counters, `heap_free`,
  `heap_min`, `arena_used`). The password is never sent back (`(set)`).
  Program output never carries `\x02` on serial (written as `?`), so a
  program cannot fake a reply; buffers that held a frame are zeroed after
  each command, and `get wifi.password` reads only the value's length.
  Serial output does not block the VM when no host reads the port: IDF's
  driver-mode stdio tries a full TX ring once for 50 ms and then drops
  (checked: netdemo kept fetching with the port closed).
  The NVS partition is not written by `idf.py flash` or `gogo flash`, so
  settings survive reflashing.

Wi-Fi setup (once per board; `gogo config` talks to the board when exactly
one is connected and `$GOGO_CONFIG` is unset, or with `-port`):

    build/gogo config scan                      # which networks the board sees (2.4 GHz only)
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
| gogo net stack fixed state (`net_s`) | 3640 |
| config handler (scratch, line buffer; task stack 4 K) | about 2.1 K + 4 K |
| free internal heap at the boot line (before the VM; was 275 K without Wi-Fi) | 197 K |
| free internal heap running `examples/netdemo` (`gogo config stats`) | about 173 K (arena 22 K in use) |

The boot logs the driver's heap use on serial (`gogo: wifi heap: ...`).

The receive path drops frames while the single RX slot is full
(`rx_busy`); TCP's one-segment windows keep that rare.

## `/flash` (LittleFS)

`fs_lfs.c` is the `vfs` backend for `/flash`: LittleFS from the
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
  `LFS_NO_MALLOC` and with `malloc` / `free` renamed to `fs_lfs.c`'s
  NULL-returning stubs, so its object imports no `malloc` / `free`. (The
  component's `CONFIG_LITTLEFS_MALLOC_STRATEGY_DISABLE` cannot be used: its
  unused VFS wrapper does not compile with it.) `CONFIG_LITTLEFS_ASSERTS=n`:
  an internal assert on a corrupt partition would abort at every boot.
- Fixed state (logged at boot as `gogo: fs state N bytes`, at most 3072,
  checked at compile time in `fs_lfs.c`): 1060 bytes = vfs tables 700 (4
  slots of 168: an `lfs_file_t`, its `lfs_file_config` and cache, plus slot
  header) + LittleFS 360 (`lfs_t`, `lfs_config`, two 64-byte caches, the
  16-byte lookahead, the partition pointer). An open file charges its owner
  160 bytes.
- Size: config `fs.flash.size` (bytes, `K` or `M` suffix, rounded down to
  4 KB, clamped to 64 KB .. the partition; unset = the whole partition,
  5056 KB; a bad value is said on the console and means the whole
  partition). At boot the size is read from the superblock; a larger
  setting grows the file system in place (`lfs_fs_grow`, files kept); a
  smaller one keeps the current size and prints `gogo: /flash shrink needs
  format (fs.flash.format=yes)`. Setting `fs.flash.format` to `yes` formats
  once at the set size at the next boot (`gogo: formatting /flash
  (fs.flash.format=yes)`; everything on `/flash` is lost) and removes the
  key; the key is removed before formatting, and if that fails nothing is
  formatted (`gogo: could not clear fs.flash.format; /flash not formatted
  (gogo config rm fs.flash.format)`), so a stuck key cannot wipe `/flash`
  at every boot. A blank or unreadable partition (mount says corrupt or
  invalid; first boot) is formatted: `gogo: formatting /flash` (LittleFS
  logs `Corrupted dir pair at {0x0, 0x1}` first). "Invalid" also covers a
  file system from a newer LittleFS on-disk version or with a larger name
  limit, so a firmware downgrade can wipe `/flash` (PLAT-5, deferred). Any other mount error
  (flash I/O), or a file system bigger than the partition, leaves `/flash`
  unmounted (every path `NotMounted`) with its data untouched and a console
  line. Serial only: `gogo: /flash <N> KB (partition 5056 KB)`.

      build/gogo config set fs.flash.size 1M
      build/gogo config set fs.flash.format yes   # only to shrink, or to wipe
      build/gogo config rm fs.flash.size          # back to the whole partition

- Measured on the M5StickS3: filling the whole partition with 4 KB writes
  runs at about 68 KB/s (5 MB in 75 s) and ends in `NoSpace`; removing the
  file returns `Usage` to its earlier value. A fresh file system uses
  24 KB (root, `kv/` and the demo's log directory included).

## `/sd` (FAT, Cardputer ADV)

`fs_fat.c` is the `vfs` backend for `/sd` on boards with an SD slot: FatFs
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
  at compile time in `fs_fat.c`): 2356 bytes = vfs 700 + LittleFS 360 +
  `fs_fat.c` 1296 (`FATFS` 572 with its 512-byte window, `sdmmc_card_t`
  204, the 512-byte bounce sector). A `FIL` is 80 bytes (slot 160).

Firmware cost of storage (vfs + B_FS + LittleFS + `fs_lfs.c`): `gogo.bin`
844272 -> 866880 bytes (+22.1 KB, LittleFS asserts off); free internal
heap at the boot line 197672 -> 196736 (-936, measured with asserts on;
the fixed state is in `.bss`).

Input build switches (Kconfig, `main/Kconfig.projbuild`; `idf.py menuconfig`
under "gogo", or `CONFIG_...=n` lines in an `SDKCONFIG_DEFAULTS` overlay):

- `GOGO_KBD` (default y): the Cardputer's TCA8418 keyboard. Off: `port.Open("kbd")`
  returns ok=false, `input_esp.c` has no keyboard code, and `i2c_bus.c` is not
  built on the Cardputer, so the IDF I2C master driver drops out of the link.
  No effect on the StickS3 (no keyboard).
- `GOGO_BATTERY` (default y): the battery reader (Cardputer ADC, StickS3 M5PM1).
  Off: `port.Open("power")` returns ok=false and no ADC or PM1 battery code is
  linked. The StickS3 still uses I2C for the PM1 LCD rail when the console is on.

**Warning:** a changed Kconfig default or an `SDKCONFIG_DEFAULTS` overlay does
not apply to a build directory that already has an `sdkconfig`: the existing
values win. Use a fresh `-B` directory, change the value with `idf.py
menuconfig`, or delete that build directory's `sdkconfig` first.

Both off is the Cardputer's smallest image (no input hardware); disabled
behaves like absent hardware. Example, own build directory per variant:

    printf 'CONFIG_GOGO_KBD=n\nCONFIG_GOGO_BATTERY=n\n' > /tmp/noinput.defaults
    idf.py -C platform/esp32s3 -B platform/esp32s3/build-cp-off -DGOGO_BOARD=cardputer \
        -DSDKCONFIG=build-cp-off/sdkconfig \
        -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.cardputer;/tmp/noinput.defaults" build

`gogo.bin` sizes (ESP-IDF 6.1.1): Cardputer default 922416 B, Cardputer both
off 897200 B, StickS3 default 870288 B, StickS3 `GOGO_BATTERY=n` 870080 B.

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
    build/gogo build examples/netdemo -o netdemo.gogo
    build/gogo flash netdemo.gogo

The screen shows `gogo: wifi connected ...`, then `gogo: ip <addr> (gateway
<g>)` and the page URL; open `http://<addr>/` in a browser on the same LAN.
The page (Chinese/English) shows the KEY1/KEY2 counts, fetches ok/total and
uptime, and refreshes every 5 s; the device also prints an `http.Get` result
every 10 s. There is no HTTPS (TLS is deferred, see `docs/m5-followups.md`).
