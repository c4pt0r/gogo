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
capture starts at boot. Only one program can use the serial port at a time.

## Flash layout (`partitions.csv`, 8 MB)

| name    | type/subtype | offset   | size     |
|---------|--------------|----------|----------|
| nvs     | data/nvs     | 0x9000   | 0x6000   |
| factory | app/factory  | 0x10000  | 0x200000 |
| gogo    | data/0x40    | 0x210000 | 0x100000 |

The `gogo` partition holds a 16-byte little-endian header
`{0x4D494747 "GGIM", image_bytes, crc32_ieee(image), 0}` and the unchanged
v1 image. At boot the firmware checks the magic, the length (multiple of 4,
at most partition size - 16) and the CRC, then mmaps the partition and runs
the image in place. Console messages:

- `gogo: no image`: no header (blank partition, or not written by `gogo flash`)
- `gogo: bad image (<reason>)`: length, crc, or an `image_load` error
- `gogo <version>: image <N> bytes, arena <cap> bytes`: boot line, then the program runs
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
  pins: 19-20 (USB), 26-32 (flash), 21, 38-41, 45 (LCD), 47-48 (M5PM1 I2C).
- `image_part.c`: the partition check and mmap above.
- `main.c`: the VM runs in a FreeRTOS task pinned to CPU1.

`platform/common/` (console grid, font) is not compiled yet; the hook is in
`main/CMakeLists.txt`.

`examples/keys` is an on-device gpio check: it prints KEY1 (G11) edges and
KEY2 (G12) presses.
