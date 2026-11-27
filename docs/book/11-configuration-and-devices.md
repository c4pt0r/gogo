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
	n, err := strconv.Atoi(v)
	if err != nil {
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

A program started on your computer has one more input, its command-line
arguments: `sys.Args()` returns the words after the program in `gogo run
prog a b` as a `[]string` (`a`, `b`), without the program's own name, and
an empty slice when there are none, always the case on a board (chapter
12).

## 11.2 Editing settings: gogo config

```text
gogo config [-port P | -local] set <key> <value|->
gogo config [-port P | -local] get <key>
gogo config [-port P | -local] list
gogo config [-port P | -local] rm <key>
gogo config [-port P | -local] import [-n] <file|->   # many keys at once
gogo config [-port P | -local] export                 # the store as a file for import
gogo config keys                 # the keys gogo itself reads (section 11.3)
```

Three board commands go with it (chapter 12):

```text
gogo device [-port P] info       # the firmware the board runs: version, board, image format
gogo device [-port P] scan       # Wi-Fi networks the board sees (2.4 GHz)
gogo device [-port P] stats      # network counters and free memory on the board
```

Keys are 1 to 15 characters of `a-z 0-9 . _` (15 is the limit of the
board's storage); values are at most 1000 bytes, without newlines. A value
of `-` reads the value from standard input, which is how you enter a
password without leaving it in your shell history.

`set` checks what it writes. Keys under the prefixes `wifi.`, `net.`,
`fs.`, `code.` and `gogo.` belong to gogo: such a key must be one of the
keys in section 11.3, and its value must have the key's type, so a typo
is caught when you type it rather than at the next boot:

```text
$ gogo config set fs.flash.size 64KB
gogo config: bad value for fs.flash.size (size): want bytes, with an optional K or M suffix, like 65536, 64K or 1M
$ gogo config set net.adress 10.0.0.9
gogo config: net.adress is not a key gogo knows, and net.* keys belong to gogo (pick another prefix for your own)
```

Any other key is yours, with any value. Keys written before this check
existed may hold upper case letters or `-`: `get` and `rm` still take
them, `set` no longer does.

**Where the store is.** On a board, it is the firmware's non-volatile
storage (NVS); it survives reflashing the firmware and the program. On
your computer it is a text file of `key=value` lines: `$GOGO_CONFIG`, or
`~/.gogo/config` by default. `gogo config` picks one this way:

1. `-port P`: the board on that serial port.
2. `-local`: the local file.
3. Neither: the local file if `$GOGO_CONFIG` is set (naming the file
   counts as choosing it); otherwise the board, if exactly one is
   connected (a line on standard error says so).
4. Otherwise it stops with an error and writes nothing:

```text
$ gogo config set wifi.ssid home          # no board plugged in
gogo config: no board found (/dev/cu.usbmodem*); connect one, pass -port, or -local for this computer's store
$ gogo config set wifi.ssid home          # two boards plugged in
gogo config: several boards found (/dev/cu.usbmodem1101, /dev/cu.usbmodem1201); pick one with -port, or -local for this computer's store
```

So with one board plugged in, `gogo config set ...` configures that
board, and a setting never lands somewhere you did not mean. This
applies to every operation that uses a store; `gogo config keys` needs
none.

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
$ gogo device scan
gogo: no board found (/dev/cu.usbmodem*); connect it or pass -port
```

`get` and `list` never print the Wi-Fi password or `node.token`, only
`(set)`.

> **Note.** On your computer the store is a plain text file (readable by
> you only), and `gogo config -local set wifi.password -` writes the
> password into it unencrypted. The tool writes a secret (`wifi.password`,
> `node.token`) there only when you chose the local store (`-local` or
> `$GOGO_CONFIG`), and then warns:
> `gogo config: warning: wifi.password is kept in plain text in ...`.
> Set Wi-Fi credentials on the board (with it connected, or `-port`).

**Many settings at once: import and export.** Keep the settings in a
file of the store's own format, one `key=value` per line; lines starting
with `#` and blank lines are skipped, and a value runs to the end of the
line (spaces and `=` included):

```text
# kitchen.cfg: the kitchen sensor
wifi.ssid=home
net.ip=192.168.1.20
net.gw=192.168.1.1
demo.url=http://192.168.1.10:8080/ingest?board=kitchen
device.name=kitchen sensor
```

`import` sets every key of the file on the store `set` would use (the
same rules, `-port` and `-local` included), in file order:

```text
$ gogo config import kitchen.cfg
gogo config: board on /dev/cu.usbmodem1101 (-local for this computer's store)
set wifi.ssid
set net.ip
set net.gw
set demo.url
set device.name
imported 5 keys to board /dev/cu.usbmodem1101
```

Every line is checked as `set` checks it first, and a key may appear
only once; if any line is wrong, nothing is written and every bad line
is named:

```text
$ gogo config import kitchen.cfg
gogo config: nothing imported:
  kitchen.cfg:3: bad value for net.gw (ip): want a dotted IPv4 address like 192.168.1.20
  kitchen.cfg:6: demo.url is set again (first on line 4)
```

`-n` (or `-dry-run`) checks the file and prints what would be set,
writing nothing; `import -` reads the file from standard input. A
secret in the file is shown as `set wifi.password (set)`, never with its
value. A board takes the keys one by one: if it refuses one (its storage
full, say), the import stops there and the error lists the keys written
and those not written. The local store is written in one go.

`export` prints the store as such a file, so one board's settings can be
copied to another. Secrets are not exported (the board never sends the
Wi-Fi password back); they appear as comments to fill in by hand:

```text
$ gogo config -port /dev/cu.usbmodem1101 export > kitchen.cfg
$ cat kitchen.cfg
# gogo config export from the board /dev/cu.usbmodem1101
demo.url=http://192.168.1.10:8080/ingest?board=kitchen
device.name=kitchen sensor
net.gw=192.168.1.1
net.ip=192.168.1.20
# wifi.password=(set, not exported)
wifi.ssid=home
$ gogo config -port /dev/cu.usbmodem1201 import kitchen.cfg
```

The board talks to `gogo config` over the USB serial console, so close
`gogo monitor` first. Program output cannot fake the board's answers.

## 11.3 Keys gogo uses

| Key | Type | Used by | Meaning |
|---|---|---|---|
| `wifi.ssid`, `wifi.password` | string (at most 32 and 64 bytes) | firmware | the Wi-Fi network; the board reconnects at once when they change |
| `net.ip`, `net.mask`, `net.gw`, `net.dns` | ip (`a.b.c.d`) | `gogo/dhcp` | a static address instead of DHCP (`net.mask` defaults to 255.255.255.0) |
| `fs.flash.size` | size | firmware | size of `/flash` (chapter 9) |
| `fs.flash.format` | `yes` | firmware | deprecated: format `/flash` once at the next boot; use `gogo device format-flash` |
| `ports.post_bytes` | size | firmware, `gogovm`, at boot | the bytes the board ports' posts (book 22.7) may queue at once: 16 KB on a board, 1 MB on the host, at most 64 MB; a post past it is `GOGO_EFULL` |
| `code.mem` | size | VM (`gogo/code`), at boot | budget for loaded module code: 64 KB on a board, 16 MB on the host (chapter 14) |
| `node.token` | string (at most 200 bytes) | `gogo/node` | the shared secret of distributed actors (chapter 20); never printed |
| `node.name` | string (at most 32 bytes) | `gogo/node` | this node's name when a program gives none |
| `vm.actor_memory` | size | VM, at boot | an actor's default memory limit (heap, stack and mailbox; `SpawnOpt` sets one actor's): 16 KB on a board, at most 256 KB; 1 MB on the host, at most 1 GB (`gogovm --actor-memory`); at least 1 KB |
| `vm.memory` | size | VM, at boot | memory for all actors together: on a board the 160 KB arena, which is also its most; no limit on the host (`gogovm --memory`); 0 is no limit |
| `vm.max_actors` | int | VM, at boot | most live actors: 1024 on a board, 16384 on the host (`gogovm --max-actors`), at most 65520 |
| `vm.max_ports` | int | VM, at boot | most ports open at once: 16 on a board, 1024 on the host (`gogovm --max-ports`), at most 2048; the table grows as ports open |
| `node.max_links` | int | `node` port, at boot | most node links: 8 on a board, 512 on the host (`gogovm --max-links`), at most 2048 |
| `vm.handles` | `N` or `N/M` | VM, at boot | off-heap `Bytes` handles in the whole VM, and (`M`) for one actor: 64/16 on a board (at most 1024), 4096/256 on the host (at most 65535; `gogovm --bytes-handles`); without `M`, the default's, at most `N` |

A size is a number of bytes with an optional `K` or `M` suffix (`65536`,
`64K`, `1M`); the VM's keys (`vm.*`, `code.mem`) also take `G`. The VM
reads its keys once, when it starts (restart the board after a change): a
gogovm flag, or a field of an embedding host's `gogo_config`, beats the
key, and the key beats the platform's default. A value past the
platform's most is cut to it with one line on the console, and one that
is not a number is ignored with a line; `gogo config set` checks the
ranges above before writing, and `gogo device info` (or `gogovm --info`)
shows what the VM runs with (section 6.2 has the table per platform).
`gogo config keys` prints this table. The `wifi.`, `net.`,
`fs.`, `code.`, `gogo.`, `node.` and `vm.` prefixes are reserved for keys gogo adds
later; anything else is yours. A dotted prefix per program (`demo.url`,
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
- `gogo: unsupported image format 16.0 (this firmware runs 15.0 to 15.3)`:
  the image was built for another major, or a newer minor than the
  firmware's: update the firmware (or build with a matching `gogo`).
- `gogo: image requires kbd, which this VM lacks`: the program opens a
  driver (or uses a mount such as `/sd`) this firmware was built without
  (`gogo firmware`, chapter 12).

When the program ends: `gogo: vm exited (<code>), arena used <bytes>`, and
the board idles until reset. Wi-Fi status lines (`gogo: wifi connected`,
`gogo: ip ...`, `gogo: network down`) appear on the screen too.
