# 12. Toolchain Reference

Everything is done with one command, `gogo`: prebuilt in each release
(`gogo-<os>-<arch>`, chapter 2), or built with `go build -o build/gogo
./cmd/gogo`. It runs on macOS and Linux; on Windows, `setup` flashes the
firmware and programs, but the serial console commands (`monitor`,
`config` on a board, and the Wi-Fi and first-program steps of `setup`)
are not supported.

```text
gogo build <dir> [-o out.ggb]
gogo run [--key c=pin]... [--fs-root DIR] <dir|file.ggb>
gogo dis <file.ggb>
gogo flash [-port P] [-esptool path] app.ggb
gogo monitor [-port P] [-t seconds] [-until text] [-reset]
gogo firmware -board sticks3|cardputer [-o dir] [-build] [-with list] [-without list] files...
gogo config [-port P | -local] set <key> <value|-> | get <key> | list | rm <key> | scan | stats
gogo setup [-port P] [-board sticks3|cardputer] [-version vX.Y.Z] [-release-url URL|DIR] [-esptool path] [-offline] [-skip-wifi] [-new-layout] [-yes] [-dry-run]
gogo version
```

## gogo build

```text
gogo build <dir> [-o out.ggb]
```

Compiles the `package main` in `<dir>` (all its `.gg` files) and the gogo
libraries it imports into one image file. Without `-o` the image is named
after the directory, `<dir>.ggb`, in the current directory. The image contains bytecode and constants and runs unchanged on
the host VM and on any board.

Errors are printed as `file:line:col: message` and the exit status is 1.
Messages from Go's type checker look like Go's (`declared and not used: x`);
messages about the subset start with `gogo:`:

```text
main.gg:9:2: gogo: captured variable must not be reassigned
```

Type errors are reported first; subset errors appear once the program
type-checks. Chapter 3 lists the subset errors.

Only `.gg` files are read: `.go` files in the directory are ignored, and a
directory with `.go` files but no `.gg` file is an error that says to
rename them (`no .gg files, only .go files: ...`). gogo sources were `.go`
files before; `.gg` is Go syntax, so map it to Go in your editor for
highlighting (chapter 2, *Editor support*); gopls does not apply.

### The `requires` word and `//gogo:optional`

The image also records the capabilities it needs (its `requires` word,
appendix F): the compiler infers them from the imports, from every
`port.Open` with a constant driver name, and from constant `/sd` paths, in
`package main` and in every library compiled in. A firmware that lacks one
refuses the image at boot (`code.Load` returns `Unsupported`, chapter 14).

A library can declare that it uses some drivers only softly: it opens them
only when a call asks for that at run time, and reports their absence
(`port.Open` returns `ok=false`) as an ordinary failure of that call. It
says so with a directive line in any of its files, conventionally the last
line of the package doc comment, after an empty `//` line (where gofmt puts
directives):

```go
// Package http is a small HTTP/1.x client and server...
//
//gogo:optional tls
package http
```

- The listed drivers add no capability **for that package's own**
  `port.Open` calls. The same driver opened by `package main`, by a
  package that imports this one, or by any other library still counts: a
  program that calls `port.Open("tls", ...)` itself requires `tls`.
- The names must be drivers that carry a capability (`imu ir_rx ir_tx kbd
  mic netif power spk tcp tcp_listen tls udp`). An unknown name, an empty
  list, a misspelt `//gogo:` directive, or the directive in `package main`
  is a compile error, for example
  `a.gg:3:1: gogo: //gogo:optional: unknown driver "tsl" (want one of ...)`.
- `gogo/http` declares `tls` (an http-only program runs on a firmware
  without TLS, where an https request fails), and `gogo/sound` declares
  `mic spk` (`Record` returns false without a microphone, `Play` without a
  speaker).
- The directive is a convention for library authors; the compiler does not
  enforce it. A library can still hand `main` a function that opens the
  driver; that open then fails cleanly at run time (`ok=false`) on a
  firmware without the driver.
- `gogo firmware` (below) chooses features from `requires`, so it does not
  turn a soft driver on by itself; `tls`, `mic` and `spk` are on by default,
  and `-without` makes those calls fail at run time instead.

## gogo run

```text
gogo run [--key c=pin]... [--fs-root DIR] <dir|file.ggb>
```

Runs a program on the host VM. Given a directory it builds it first (to a
temporary image); given a `.ggb` file it runs that image. The exit status
is the VM's: 0 when the VM ran until nothing was left to do, 1 if the
image needs a capability the host VM lacks (for example `tls` in a VM
built without mbedTLS), 2 if the image could not be loaded. A build error
is status 1 too.

> **Note.** A crash, even of the `main` actor, does not change the exit
> status: crashes are reported on standard error and the VM goes on until
> it is idle. There is no way for a program to set the exit status, so
> scripts must look at the output to tell success from failure.

Options:

- `--fs-root DIR`: back `/flash` with `DIR/flash` (created if missing) and
  `/sd` with `DIR/sd` (only if it exists). Without it there is no storage.
  Programs stay inside those directories: symlinks in them are not
  followed (opening one, or a path through one, is `BadPath`; `ReadDir`
  lists it as a file), and neither are FIFOs or devices. On a host file
  system that ignores case (macOS by default) the media ignore case too.
  The VM program `build/gogovm` also takes `--fs-quota BYTES` (tests
  only): each medium may then hold that many bytes of files, counting
  what is already there; deletes give room back, and `fs.Usage` reports
  (used, quota). See section 16.5.
- `--key c=pin`: in a terminal, pressing the character `c` presses the
  simulated GPIO input `pin` (the level goes to 0, then back to 1). Up to
  16 keys.

**Standard input** feeds the simulated hardware:

- If it is **not a terminal** (a file or a pipe), it is a script, read one
  line at a time whenever the VM is idle and a port that can produce events
  is open:

  | Line | Meaning |
  |---|---|
  | `<pin> <0\|1>` | a GPIO input changes level |
  | `key <name>` | a key press and release (`kbd` port) |
  | `keydown <name>`, `keyup <name>` | press or release only, for modifiers |
  | `battery <mV> <charging> <external>` | a battery sample (`power` port) |
  | `imu sample <gx> <gy> <gz> <ax> <ay> <az>`, `imu gesture <kind> [value]` | IMU data (`imu` port, chapter 15) |
  | `ir rx <us> <us> ...` | a received infrared frame (`ir_rx` port, chapter 15) |
  | `mic tone <hz> <ms>`, `mic silence <ms>` | microphone input (`mic` port, chapter 16) |
  | `sleep <ms>` | pause before the next line |

  Unknown lines are reported on standard error and skipped. Key lines are
  dropped while no `kbd` port is open. At the end of the script no more
  input can arrive, so an idle program can end.
- If it **is a terminal**, `--key` characters drive GPIO pins and, while a
  `kbd` port is open, other keys become key presses. Ctrl-D ends the
  input; Ctrl-C stops the program (and restores the terminal).

**Output.** `println` writes to standard output. Crash reports and the
simulated GPIO outputs (`gpio: pin 5 = 1`) go to standard error.

**Environment.** `GOGO_VM` names the VM to use (default: `build/gogovm` of
the gogo tree). `GOGO_CONFIG` names the configuration file that
`config.Get` reads (default `~/.gogo/config`). `GOGO_UNIX_TIME_MS` starts
the wall clock (`sys.Time`, chapter 10.8) at that Unix time in ms instead of
the computer's clock; `0`, or a time earlier than the VM's build, starts it
unknown, as on a board.

There is no network on the host (chapter 10).

## gogo dis

```text
gogo dis <file.ggb>
```

Prints the bytecode of an image, function by function. Useful to see what
the compiler made of your code, or to compare two builds:

```text
$ gogo build docs/book/examples/02-hello -o hello.ggb
$ gogo dis hello.ggb
func main (params=0 regs=4):
  0000  LOADK r1, "hello, gogo"
  0002  LOADK r2, 42
  0004  LOADK r3, true
  0006  BUILTIN r0, r1, 3, PRINTLN
  0008  LOADK r0, nil
  0010  RET r0
```

The VM is register based: each function has a fixed number of registers,
which live in a stack frame in the actor's memory.

## gogo flash

```text
gogo flash [-port P] [-esptool path] app.ggb
```

Writes an image (built with `gogo build`; `flash` does not take a
directory) to the board's program partition at 0x210000, with a header and
a CRC that the firmware checks at boot. Needs the board's gogo firmware to
be installed already (chapter 2).

- `-port P`: the serial port. Default: the only `/dev/cu.usbmodem*`; with
  none or several, `gogo flash` stops and asks for `-port`.
- `-esptool path`: the esptool to run. Default: `esptool` or `esptool.py`
  from `PATH` (`. ~/esp/esp-idf/export.sh` puts it there).

`gogo flash` refuses files that are not gogo images, images of another
format version (`rebuild it with gogo build`) and images over the 1 MB
partition.

## gogo monitor

```text
gogo monitor [-port P] [-t seconds] [-until text] [-reset]
```

Copies the board's console to standard output.

- `-t seconds`: stop after this long (default 10).
- `-until text`: stop after the first line containing `text`, e.g.
  `-until "gogo: vm exited"`.
- `-reset`: reset the board first, so the capture starts at boot. Without
  it, opening the port never resets the board.
- `-port P`: as for `gogo flash`.

It is not interactive. Only one program can have the serial port open, so
stop `gogo monitor` before `gogo flash` or `gogo config`.

## gogo firmware

```text
gogo firmware -board sticks3|cardputer [-o dir] [-build] [-with list] [-without list] files...
```

Chooses which optional parts the firmware needs to contain for a set of
images and modules, so a build does not carry a network stack or an SD
driver nobody uses. It reads the capabilities each file declares (the
`requires` word that `gogo build` writes), takes their union, and writes
`<dir>/sdkconfig.gogo`, a list of `CONFIG_GOGO_*` lines, then prints the
`idf.py` command that builds with it. It never flashes.

- Feature names: `net kbd power sd exfat cjk imu irtx irrx mic spk tls`.
  `net`, `kbd` and `power` are on only when a file requires them (or
  `-with` adds them); a file that opens a port by a name only known at run
  time requires every port the board has. The others are on by default on
  boards that have them: `sd`, `imu`, `irtx`/`irrx` (the infrared sender
  and receiver), `mic`/`spk` and `tls` (the TLS client, chapter 17) are
  capabilities too, so a file that needs one keeps it on, while `exfat` (exFAT media) and `cjk` (the Chinese font)
  are not. `irtx` and `irrx` share one firmware switch, as do `mic` and
  `spk`, so they go on and off together.
- `-with a,b` adds features. `-without a,b` removes them; a feature that a
  file requires cannot be removed (error). `exfat` goes off with `sd`, and
  `tls` with `net`.
- A required feature the board lacks is an error that names it, for example
  `kbd` or `sd` on the `sticks3`.
- `-o dir`: the build directory and where `sdkconfig.gogo` goes (default
  `build-fw-<board>`). `-build` runs the printed command (from the gogo
  repository root, with ESP-IDF exported, chapter 2).

The tool owns the build directory. ESP-IDF would let an existing
`<dir>/sdkconfig` override a changed `sdkconfig.gogo`, so if that file exists
and any `CONFIG_GOGO_*` value in it differs from the new features, the tool
deletes it and prints a line saying so. If nothing differs it is kept and the
next build stays incremental. A lone `-` argument is an error.

## gogo config

```text
gogo config [-port P | -local] set <key> <value|->
gogo config [-port P | -local] get <key>
gogo config [-port P | -local] list
gogo config [-port P | -local] rm <key>
gogo config [-port P] scan
gogo config [-port P] stats
```

Edits the configuration store on the board or in the local file. See
chapter 11 for how the store is chosen, the key rules and the keys gogo
uses. `scan` lists the 2.4 GHz networks the board sees; `stats` prints the
board's network counters and free memory (on a firmware built without
`net`, only the memory figures). Both need a board.

## gogo setup

```text
gogo setup [-port P] [-board sticks3|cardputer] [-version vX.Y.Z] [-release-url URL|DIR]
           [-esptool path] [-offline] [-skip-wifi] [-new-layout] [-yes] [-dry-run]
```

Takes a board from new to running a program, in six steps (chapter 2
shows a run):

1. finds esptool (version 5 or newer) and the board's serial port
   (`/dev/cu.usbmodem*` on macOS, `/dev/ttyACM*` on Linux; with several,
   it asks);
2. identifies the board with esptool (letting it detect the chip, and
   reading the partition table now on the board): an ESP32-S3 with
   embedded PSRAM is the M5StickS3, one without is the Cardputer ADV; you
   confirm, and any other chip is refused;
3. gets the board's firmware: downloads it from a release, checks every
   file against the release's `SHA256SUMS` (a mismatch stops setup), and
   caches it in the user cache directory (`gogo/release/<sha256>`); without
   a release it builds the firmware locally (below);
4. flashes the bootloader, partition table and app at the offsets in the
   firmware's `flasher_args.json`, then resets the board with a watchdog
   reset (the Cardputer ADV does not come back from esptool's default
   reset);
5. scans for Wi-Fi networks on the board, and writes the chosen network
   and the password (read without echo) like `gogo config set`;
6. flashes a small hello program like `gogo flash` and watches the
   console until it prints.

**What it never does.** It never erases the chip and never writes a merged
image: esptool only ever runs `read-flash`, `write-flash` and `version`.
The partition table must be at 0x8000. Every write must lie in the
bootloader area (below 0x8000), on the partition table sector or inside an
app partition of the partition table being written, and no write, rounded
out to whole 4 KB flash sectors, may touch a protected range: the NVS
partition (where the board keeps its settings, Wi-Fi included) of the
table being written, of the table now on the board, and of the gogo
partition table (0x9000..0xF000), all three. Setup checks this before it
runs esptool. If the board's table puts NVS elsewhere than the new one,
or the partition table sector holds data that is not a readable table
(blank flash is fine), setup stops unless `-new-layout` is given. The Wi-Fi password is never
printed, logged or stored on the computer.

**Which release.** `-release-url` (or `$GOGO_RELEASE_URL`) names the files
of one release: an `http(s)` URL or a local directory, such as one filled
by `gh release download` or `tools/release.sh`. Otherwise `-version vX.Y.Z`
picks a GitHub release of `c4pt0r/gogo`; otherwise the release matching
this `gogo` (`gogo version`) if that is a release tag (`vX.Y.Z`, or with
`-rcN`, `-beta.N`, `-alpha.N`), else the latest, pinned to its tag on the
first request so every file comes from one release. Drafts are not
visible to setup until published (`platform/esp32s3/README.md`).
The assets setup reads are:

| Asset | What |
|---|---|
| `SHA256SUMS` | the SHA-256 of every other asset |
| `<board>-flasher_args.json` | ESP-IDF's list of files and offsets for the board (`sticks3`, `cardputer`) |
| `<board>-bootloader.bin`, `<board>-partition-table.bin`, `<board>-gogo.bin` | the files it lists, named `<board>-<file name>` |
| `gogo-<os>-<arch>[.exe]` | the gogo command, for `darwin`, `linux`, `windows` on `amd64`, `arm64` |
| `LICENSE`, `THIRD_PARTY_NOTICES.md`, `font-licenses.zip`, `LICENSE-APACHE-2.0.txt` | the licenses that go with the binaries (setup does not read them) |

To try a release layout before publishing it, point setup at the
directory, or serve it over HTTP:

```sh
tools/release.sh v0.1.0                 # build/release/v0.1.0/
gogo setup -release-url build/release/v0.1.0
python3 -m http.server -d build/release 8765 &
gogo setup -release-url http://127.0.0.1:8765/v0.1.0
```

**Local firmware build.** With `-offline`, or when the release cannot be
downloaded (and you agree), setup builds the board's default firmware
with `gogo firmware -build` and every feature the board has, into
`build/setup-<board>` of the gogo source tree. It needs to run inside the
gogo source tree and ESP-IDF v6.1.1 (`idf.py` on `PATH`, else
`$IDF_PATH/export.sh` or `~/esp/esp-idf/export.sh`, which it sources).

Options:

- `-port P`, `-esptool path`: as for `gogo flash`.
- `-board sticks3|cardputer`: skip the board question.
- `-skip-wifi`: leave the board's Wi-Fi settings as they are.
- `-new-layout`: flash although the board's partition table keeps NVS
  elsewhere than the new firmware's, or cannot be read. Neither NVS area
  is written, but the old settings are then not used, and the new
  firmware (or a later one) may reuse the space where they were.
- `-yes`: take the default answer to every question, without asking;
  the Wi-Fi step is then skipped, and a board esptool cannot tell apart
  needs `-board`.
- `-dry-run`: run only what reads (esptool's chip check, the download,
  the Wi-Fi scan) and print the esptool commands and settings that would
  be written, without writing anything.

On failure setup prints the problem and a `fix:` line, and stops; it is
safe to run again.

## gogo version

Prints the version of `gogo` (`dev` unless built by `tools/release.sh`)
and the image format version it writes.

## Building the firmware

The firmware is an ESP-IDF project in `platform/esp32s3`, built with
`idf.py` (ESP-IDF v6.1.1). See chapter 2 for the commands and
`platform/esp32s3/README.md` for build options (`GOGO_BOARD`,
`GOGO_CONSOLE=0`, and the `CONFIG_GOGO_*` feature switches `NET`, `KBD`,
`BATTERY`, `SD`, `FS_EXFAT`, `FONT_CJK`, `IMU`, `IR`, `AUDIO`, `TLS`) and hardware
details. To choose the
switches from what your programs need, use `gogo firmware` (above).
`gogo setup` flashes a prebuilt firmware from a release instead.

## Running the book's examples

```sh
make vm && go build -o build/gogo ./cmd/gogo
docs/book/examples/run.sh              # all examples
docs/book/examples/run.sh 04-call      # one
```

`run.sh` runs each example the way the book shows (with its `input`,
`fsroot` and `config` files) and compares the output with the
`expected.out` and `expected.err` files next to it.
