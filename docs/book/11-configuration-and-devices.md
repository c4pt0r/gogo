# 11. Configuration and Devices

A device needs settings that are not part of the program: the Wi-Fi
network, a fixed IP address, the URL of a server, a calibration value.
gogo keeps them in a **configuration store** of string keys and values,
edited from your computer with `gogo config` and read by programs with
`sys.Config`.

## 11.1 Reading settings: sys.Config

```go
package sys

func Config(key string) (value string, ok bool) // ok is false if the key is not set
```

Values are strings; convert them with `gogo/strconv`. Always have a
default, so the program also runs where the key is not set:

```go
// Read settings from the config store. Set them with "gogo config set
// blink.ms 250" (on the board when one is connected, else in
// ~/.gogo/config). Keys that start with "wifi." are never visible to
// programs.
package main

import (
	"gogo/strconv"
	"gogo/sys"
)

// intSetting returns the key's value as an int, or def.
func intSetting(key string, def int) int {
	v, ok := sys.Config(key)
	if !ok {
		return def
	}
	n, ok := strconv.Atoi(v)
	if !ok {
		println("config:", key, "is not a number:", v)
		return def
	}
	return n
}

func main() {
	println("blink.ms =", intSetting("blink.ms", 500))
	println("retries =", intSetting("retries", 3))
	name, ok := sys.Config("device.name")
	println("device.name =", name, ok)
	_, ok = sys.Config("wifi.ssid")
	println("wifi.ssid visible:", ok)
}
```

With this store (on the host, the file named by `$GOGO_CONFIG`):

```text
blink.ms=250
retries=lots
device.name=kitchen sensor
wifi.ssid=home
```

```text
blink.ms = 250
config: retries is not a number: lots
retries = 3
device.name = kitchen sensor true
wifi.ssid visible: false
```

Keys starting with `wifi.` belong to the firmware: programs never see them,
whatever is stored.

## 11.2 Editing settings: gogo config

```text
gogo config [-port P | -local] set <key> <value|->
gogo config [-port P | -local] get <key>
gogo config [-port P | -local] list
gogo config [-port P | -local] rm <key>
gogo config [-port P] scan       # Wi-Fi networks the board sees (2.4 GHz)
gogo config [-port P] stats      # network counters and free memory on the board
```

Keys are 1 to 15 characters of `A-Z a-z 0-9 . _ -` (the limit of the
board's storage); values are at most 1000 bytes, without newlines. A value
of `-` reads the value from standard input, which is how you enter a
password without leaving it in your shell history.

**Where the store is.** On a board, it is the firmware's non-volatile
storage (NVS); it survives reflashing the firmware and the program. On
your computer it is a text file of `key=value` lines: `$GOGO_CONFIG`, or
`~/.gogo/config` by default. `gogo config` picks one this way:

1. `-port P`: the board on that serial port.
2. `-local`: the local file.
3. Neither: the local file if `$GOGO_CONFIG` is set; otherwise the board,
   if exactly one is connected (a line on standard error says so);
   otherwise the local file.

So with a board plugged in, `gogo config set ...` configures the board.

A session against the local file:

```text
$ export GOGO_CONFIG=/tmp/demo-config
$ gogo config set blink.ms 250
$ gogo config set device.name "kitchen sensor"
$ gogo config list
blink.ms=250
device.name=kitchen sensor
$ gogo config get blink.ms
250
$ gogo config get nope
gogo config: nope is not set
$ gogo config rm blink.ms
$ gogo config list
device.name=kitchen sensor
$ gogo config scan
gogo config: scan needs a board (-port)
```

`get` and `list` never print the Wi-Fi password, only `(set)`.

> **Note.** On your computer the store is a plain text file, and `gogo
> config set wifi.password -` writes the password into it unencrypted.
> Set Wi-Fi credentials only on the board (with a board connected, or
> `-port`), never with `-local`.

The board talks to `gogo config` over the USB serial console, so close
`gogo monitor` first. Program output cannot fake the board's answers.

## 11.3 Keys gogo uses

| Key | Used by | Meaning |
|---|---|---|
| `wifi.ssid`, `wifi.password` | firmware | the Wi-Fi network; the board reconnects at once when they change |
| `net.ip`, `net.mask`, `net.gw`, `net.dns` | `gogo/dhcp` | a static address instead of DHCP (`net.mask` defaults to 255.255.255.0) |
| `fs.flash.size` | firmware | size of `/flash` (chapter 9) |
| `fs.flash.format` | firmware | `yes`: format `/flash` once at the next boot |
| `code.mem` | `gogo/code` | budget for loaded module code, default 64 KB (`K`/`M` suffix; chapter 14) |

Anything else is yours. A dotted prefix per program (`demo.url`,
`fsdemo.mode`) keeps keys apart.

## 11.4 The two boards

| | M5StickS3 | Cardputer ADV |
|---|---|---|
| Chip | ESP32-S3 (PICO-1-N8R8, 8 MB PSRAM) | ESP32-S3 (Stamp S3A, no PSRAM) |
| Flash | 8 MB | 8 MB |
| Screen | 240 x 135 LCD (console) | 240 x 135 LCD (console) |
| Buttons | KEY1 = G11, KEY2 = G12 (`gpio`) | G0 button (`gpio`) |
| Keyboard (`kbd`) | none | 56 keys |
| Battery (`power`) | voltage, charging, external power | voltage only (charging and external are -1) |
| Storage | `/flash` | `/flash`, `/sd` |
| Firmware build | `GOGO_BOARD=sticks3` (default) | `GOGO_BOARD=cardputer` |

Both boards have the same flash layout, so `gogo flash` writes a program to
either without a board option. Their firmware differs; build the one for
your board (chapter 2).

On both boards the VM gets 160 KB of internal RAM for actors (16 KB per
actor by default) and runs on the second CPU core, while Wi-Fi and the
screen run on the first.

### Flash layout

| Partition | Offset | Size | Holds |
|---|---|---|---|
| `nvs` | 0x9000 | 24 KB | the configuration store |
| `factory` | 0x10000 | 2 MB | the firmware |
| `gogo` | 0x210000 | 1 MB | your program image |
| `gogofs` | 0x310000 | about 5 MB | `/flash` |

`gogo flash` writes only the `gogo` partition, `idf.py flash` only the
firmware. Neither touches the configuration store or `/flash`.

### Console messages

At boot the firmware checks the program image and prints one of:

- `gogo <version> (image format 15.<minor>): image <N> bytes, arena <cap> bytes, free RAM <F> (largest <L>)`:
  all is well; the program runs.
- `gogo: no image`: nothing was flashed with `gogo flash`.
- `gogo: bad image (<reason>)`: a damaged image.
- `gogo: unsupported image format 16.0 (this firmware runs 15.0 to 15.2)`:
  the image was built for another major, or a newer minor than the
  firmware's: update the firmware (or build with a matching `gogo`).
- `gogo: image requires kbd, which this VM lacks`: the program opens a
  driver (or uses a mount such as `/sd`) this firmware was built without
  (`gogo firmware`, chapter 12).

When the program ends: `gogo: vm exited (<code>), arena used <bytes>`, and
the board idles until reset. Wi-Fi status lines (`gogo: wifi connected`,
`gogo: ip ...`, `gogo: network down`) appear on the screen too.
