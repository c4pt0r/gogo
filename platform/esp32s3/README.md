# gogo on the ESP32-S3 (M5StickS3)

An ESP-IDF project whose `main` component is the ESP platform layer; it
compiles the VM core (`vm/*.c` except `check32.c`) in place from the repo
root, with the ESP values of the `vm/config.h` knobs:
`GOGO_DEFAULT_MAX_WORDS=4096`, `GOGO_TABLE_CAP=1024`, `GOGO_MAX_PORTS=16`,
`GOGO_EVQ_CAP=64`, `GOGO_ARENA_BYTES=163840` (160 KB), `-Os`.

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

The `gogo` partition holds a 16-byte little-endian header
`{0x4D494747 "GGIM", image_bytes, crc32_ieee(image), 0}` and the image
(format v4, lockstep with `IMAGE_VERSION` in `vm/image.h`; when the
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
  pins: 19-20 (USB), 26-37 (in-package flash and octal PSRAM of the
  ESP32-S3-PICO-1-N8R8), 21, 38-41, 45 (LCD), 47-48 (M5PM1 I2C).
- `image_part.c`: the partition check and mmap above.
- `main.c`: the VM runs in a FreeRTOS task pinned to CPU1.
- Screen (debug console, mirror of stdout/stderr):
  - `pm1.c`: M5PM1 (I2C G47/G48, 0x6E): I2C idle sleep and watchdog off,
    PM1 GPIO2 high = LCD rail, 200 ms settle (M5GFX/M5Unified StickS3
    sequence, cited in the file).
  - `lcd_st7789.c`: ST7789P3 135x240 used as landscape 240x135 (column
    offset 40, row offset 53, MADCTL 0x6C, inverted), SPI3 40 MHz with DMA;
    backlight G38.
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
