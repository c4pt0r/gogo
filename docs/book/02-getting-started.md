# 2. Getting Started

This chapter puts gogo on a board with one command, then builds the tools
from source, runs a first program on your computer, and shows the manual
way to flash a board.

## 2.1 The quick way: `gogo setup`

`gogo setup` takes a new M5StickS3 or Cardputer ADV to a running program:
it identifies the board, downloads the matching firmware, flashes it, sets
up Wi-Fi and runs a first program. You need:

- the board and a USB-C **data** cable (some cables only charge);
- the **`gogo` command**, prebuilt for your system;
- **esptool** 5 or newer, the ESP32 flashing tool:
  `pip install esptool` or `brew install esptool`.

You do not need Go, a C compiler or ESP-IDF.

### Install `gogo`

Each gogo release on GitHub has the command for macOS, Linux and Windows,
on amd64 and arm64: `gogo-darwin-arm64`, `gogo-darwin-amd64`,
`gogo-linux-amd64`, `gogo-linux-arm64`, `gogo-windows-amd64.exe` and
`gogo-windows-arm64.exe`. For example, on an Apple silicon Mac:

```sh
curl -L -o gogo https://github.com/c4pt0r/gogo/releases/latest/download/gogo-darwin-arm64
chmod +x gogo
sudo mv gogo /usr/local/bin/     # or anywhere on your PATH
gogo version
```

A file downloaded with a browser on macOS may need
`xattr -d com.apple.quarantine gogo` before it runs.

### Run it

Connect the board and run:

```sh
gogo setup
```

It works in six steps, says what each one does, and asks before it writes
to the board:

```text
[1/6] Checking the environment
  esptool 5.6.0 (/usr/local/bin/esptool)
  port /dev/cu.usbmodem5101

[2/6] Identifying the board
  $ esptool --port /dev/cu.usbmodem5101 --after watchdog-reset read-flash 0x8000 3072 ...
  chip: ESP32-S3-PICO-1 (LGA56) (revision v0.2), embedded PSRAM
  This looks like the M5StickS3. Right? [Y/n]

[3/6] Getting the firmware
  release: https://github.com/c4pt0r/gogo/releases/latest/download
  sticks3-flasher_args.json (899 bytes, sha256 ok)
  sticks3-bootloader.bin (18880 bytes, sha256 ok)
  sticks3-partition-table.bin (3072 bytes, sha256 ok)
  sticks3-gogo.bin (1093072 bytes, sha256 ok)

[4/6] Flashing the firmware
  0x00000000  bootloader.bin (18880 bytes)
  0x00008000  partition-table.bin (3072 bytes)
  0x00010000  gogo.bin (1093072 bytes)
  not written: NVS 0x9000..0xf000 (settings such as Wi-Fi stay)
  Flash this firmware to the M5StickS3 on /dev/cu.usbmodem5101? [Y/n]

[5/6] Setting up Wi-Fi
  Set up Wi-Fi now? [Y/n]
  scanning for networks on the board...
  1) Home Net                         ch 6 -50 dBm
  2) another network (type its name)
  Which network? [1]
  Wi-Fi password (not shown):
  saved on the board: wifi.ssid=Home Net, wifi.password=(set)
  waiting for the board to join...
  | gogo: wifi connected (Home Net, ch 6, -50 dBm)
  Wi-Fi works

[6/6] Running a first program
  hello program: 1552 bytes, to the image partition at 0x210000
  ...
  | gogo <version>: image 1552 bytes, arena 163840 bytes, free RAM <F> (largest <L>)
  | hello from gogo on the M5StickS3!
```

1. **Environment.** It finds esptool and the board's serial port
   (`/dev/cu.usbmodem*` on macOS, `/dev/ttyACM*` on Linux). With several
   ports it asks which one. On Windows, pass `-port COM5`; there setup
   flashes the firmware and the first program but skips the Wi-Fi step
   and does not watch the board's console (gogo cannot use the serial
   console on Windows).
2. **Board.** esptool reports the chip. Both boards are ESP32-S3s; the
   StickS3 has embedded PSRAM and the Cardputer ADV has none, so that is
   the guess, which you confirm (if esptool does not say, setup asks).
   Any other chip is refused.
3. **Firmware.** It downloads the board's firmware from the gogo release
   that matches your `gogo` (or the latest one), checks every file
   against the release's `SHA256SUMS`, and keeps them in your user cache
   directory (`~/Library/Caches/gogo` on macOS, `~/.cache/gogo` on
   Linux), so a second run downloads only `SHA256SUMS` again.
4. **Flash.** It writes the bootloader, the partition table and the
   firmware at the offsets the firmware build lists. It never erases the
   chip and never writes the NVS partition, where the board keeps its
   settings, so a Wi-Fi setup from before survives. If the firmware now
   on the board keeps its settings somewhere else (a board straight from
   the factory may), or its partition table cannot be read, setup stops
   and says so; `-new-layout` flashes anyway, still without writing
   either settings area. The old settings are then not used, and later
   firmware or programs may reuse the space where they were.
5. **Wi-Fi.** The board scans for networks and you pick one. The
   password is read without echo and sent only to the board; gogo never
   prints or stores it. You can skip this step and use `gogo config`
   later (chapter 11).
6. **First program.** A small program is flashed to the program
   partition; the board shows its greeting on the screen and on the
   console, where `gogo setup` waits for it.

When something fails, `gogo setup` prints what went wrong and a line
starting `fix:` that says what to do, then stops. Running it again is
always safe.

### Options

| Option | Effect |
|---|---|
| `-port P` | the board's serial port |
| `-board sticks3\|cardputer` | skip the board question |
| `-version vX.Y.Z` | that release instead of the one matching `gogo` |
| `-release-url URL\|DIR` | the files of one release from another place: an `http(s)` URL or a local directory (also `GOGO_RELEASE_URL`) |
| `-offline` | build the firmware locally instead of downloading it |
| `-skip-wifi` | leave the Wi-Fi settings as they are |
| `-new-layout` | flash even though the board keeps its settings elsewhere than the new firmware, or its partition table cannot be read (the old settings are then not used, and their space may be reused later) |
| `-yes` | accept every default without asking (Wi-Fi is then skipped) |
| `-dry-run` | read the board and download, but only print the commands that would write to it |
| `-esptool path` | the esptool to run |

Without a release to download (no network, or no such release),
`gogo setup` offers to build the firmware with ESP-IDF instead, if it is
installed (`idf.py` on `PATH`, `$IDF_PATH` or `~/esp/esp-idf`) and you run
it inside the gogo source tree. Section 2.6 has the manual commands.

The prebuilt `gogo` also compiles and flashes your own programs (section
2.5). To run programs on your computer, as most of this book does, you
also need the host VM, `gogovm`. Each release has it for macOS and Linux:
`gogovm-darwin-arm64`, `gogovm-darwin-amd64`, `gogovm-linux-amd64` and
`gogovm-linux-arm64`. Put it next to `gogo`, named `gogovm` (the asset
name works too):

```sh
curl -L -o gogovm https://github.com/c4pt0r/gogo/releases/latest/download/gogovm-darwin-arm64
chmod +x gogovm
sudo mv gogovm /usr/local/bin/   # the directory where gogo is
gogo env                         # shows the gogovm it found
```

The release VM has no `tls` port (HTTPS on the host, chapter 17); for
that, build gogo from source (section 2.2).

## 2.2 Building the tools from source

To change gogo itself, run its tests, or have a host VM with HTTPS, build the
tools from source.
### What you need

On your computer (macOS or Linux):

- **Go 1.27 or newer**, to build the `gogo` command (the compiler and
  board tools). It uses only Go's standard library.
- **A C compiler and `make`**, to build `gogovm`, the VM that runs
  programs on your computer. Any C99 compiler works (`cc`, `clang`, `gcc`).
- **The gogo source tree.**

To flash firmware by hand you also need ESP-IDF, covered in section 2.6.
You do not need it to follow most of this book: every example runs on your
computer.

### Building

From the root of the gogo source tree:

```sh
make vm                          # builds build/gogovm, the host VM
go build -o build/gogo ./cmd/gogo
```

`build/gogo` is the only command you use directly. When it runs a program it
looks for the VM in this order: `$GOGO_VM`; `gogovm` in the directory of
the `gogo` command itself (`build/gogovm` here, or wherever a release put
it); `build/gogovm` in the gogo tree around `gogo` or your current
directory. `gogo env` prints the one it would use.

Run it without arguments to see what it can do:

```text
$ build/gogo
usage: gogo build [-path dirs] [-o out.ggb] <dir|file.gg>
       gogo run [--key c=pin]... [--fs-root DIR] [-path dirs] <dir|file.gg|file.ggb>
       gogo env [-path dirs]
       gogo dis <file.ggb>
       gogo info [-path dirs] <file.ggb|dir|file.gg>
       gogo flash [-port P] [-esptool path] [-path dirs] <app.ggb|dir|file.gg>
       gogo push [-port P] [-timeout seconds] <file.ggb> [/flash/path]
       gogo monitor [-port P] [-timeout seconds] [-until text] [-reset]
       gogo firmware (-board sticks3|cardputer | -port P [-esptool path]) [-o dir] [-build] [-with list] [-without list] files...
       gogo config [-port P | -local] set <key> <value|-> | get <key> | list | rm <key> | keys | scan | stats
       gogo device [-port P] [-yes] scan | stats | format-flash
       gogo setup [-port P] [-board sticks3|cardputer] [-version vX.Y.Z] [-release-url URL|DIR] [-esptool path] [-offline] [-skip-wifi] [-new-layout] [-yes] [-dry-run]
       gogo version
```

The rest of this book writes `gogo` for `build/gogo`. Put it on your `PATH`
if you like.

## 2.3 Your first program

A gogo program is a directory holding one `package main` (in one or more
`.gg` files: gogo source, Go syntax) with a `func main()`:

```go
// The smallest gogo program: package main, func main, println.
package main

func main() {
	println("hello, gogo", 42, true)
}
```

Run it with `gogo run` and the directory:

```text
$ gogo run docs/book/examples/02-hello
```

```text
hello, gogo 42 true
```

`gogo run` compiles the directory to a temporary image and runs it on the
host VM. `println` is the only printing function: it prints its arguments
separated by spaces, followed by a newline, on standard output. (There is no
`fmt` package.)

When the program has nothing left to do (no actor can run, no timer is
pending, no input can arrive) the VM exits. The exit status says how
`main` ended: 0 when it returned, 4 when it crashed, or the code it
passed to `actor.Exit` (chapter 12 has the table). Crashes of other
actors are reported on standard error, as you will see in chapter 5,
and do not change it.

### Scripts

`gogo run` also takes a single `.gg` file, and compiles that file alone:

```text
$ gogo run hello.gg
```

A file whose first line starts with `#!` is an executable script: the
compiler reads that line as an empty one (line numbers in errors stay
right), so with

```go
#!/usr/bin/env -S gogo run
package main

func main() {
	println("hello from a script")
}
```

and `chmod +x hello.gg`, `./hello.gg` runs it (`gogo` must be on your
`PATH`). `env -S`, which splits `gogo run` into two words, is in macOS and
in GNU coreutils 8.30 and newer, but not in BusyBox (Alpine) or older
coreutils. There, name gogo by its absolute path instead, which needs no
`env`: `#!/usr/local/bin/gogo run` (the kernel passes `run` as one
argument).
The name need not end in `.gg`: `gogo run` reads any file starting with
`#!` as source. Arguments after the script are the program's: it reads
them with `sys.Args()` (chapter 12).

A single file and a directory differ: `gogo run dir` compiles every `.gg`
file in `dir` as one `package main`, so a directory of scripts, each with
its own `func main`, only runs file by file. A script can still import
packages from its directory (`import "scripts/calc"` for `scripts/calc/`
next to a script in `scripts/`) and from the search path, as below.

### Packages and GOGOPATH

A program can be split into packages of its own (section 3.1.1):
`import "app/shapes"` in a program in `app/` is the directory
`app/shapes/`. Packages that several programs share live in a directory
on the search path, by default `$HOME/gogo`:

```sh
mkdir -p ~/gogo/lib/ring                 # import "lib/ring"
export GOGOPATH=$HOME/gogo:$HOME/work/gogo-pkgs   # optional: several directories
gogo env                                 # prints the search path
```

`gogo build -path dirs` and `gogo run -path dirs` use `dirs` instead of
`GOGOPATH` for one build.

## 2.4 A first actor

The `main` function runs in the first actor. It can start more with
`actor.Spawn`, which takes a function to run, and talk to them with
`actor.Send` and `actor.Receive`:

```go
// A first actor: main spawns a greeter, sends it a name, and waits for the
// greeting to come back.
package main

import "gogo/actor"

// Greet asks the greeter to greet Name and answer to From.
type Greet struct {
	From actor.PID
	Name string
}

func greeter() {
	for {
		switch m := actor.Receive().(type) {
		case Greet:
			actor.Send(m.From, "hello, "+m.Name)
		}
	}
}

func main() {
	g := actor.Spawn(greeter)
	actor.Send(g, Greet{From: actor.Self(), Name: "world"})
	println(actor.Receive())
}
```

```text
hello, world
```

Read it in order:

1. `actor.Spawn(greeter)` starts a new actor running `greeter` and returns
   its PID (process identifier).
2. `actor.Send(g, Greet{...})` puts a `Greet` message in the greeter's
   mailbox. The message carries `actor.Self()`, main's own PID, so the
   greeter knows where to answer.
3. The greeter's `actor.Receive()` returns the message as an `any`; the
   *type switch* finds out it is a `Greet`.
4. The greeter sends a string back, and main's `actor.Receive()` returns
   it.

Chapter 4 covers all of this in detail. Notice already that `greeter` is
still waiting in `Receive` when main returns. That is fine: nothing can
ever reach it again, so the VM exits.

### Editor support

gogo source files end in `.gg`, and compiled images in `.ggb`. A `.gg`
file is Go syntax. The repository's [`editors/`](../../editors/README.md)
directory has ready-made support that gives `.gg` a `gogo` file type (Go
highlighting and indentation, plus the `//gogo:` directives and `#!`
scripts) and a black and white color scheme, *gogo mono*:

- **VS Code**: link the extension and restart VS Code,

  ```sh
  ln -s "$PWD/editors/vscode" ~/.vscode/extensions/gogo-lang
  ```

- **Vim / Neovim**: add `editors/vim` to the runtime path,

  ```vim
  set runtimepath^=/path/to/gogo/editors/vim
  ```

  and optionally `colorscheme gogo-mono`.
- **Emacs**: with `go-mode` installed, load `editors/emacs/gogo-mode.el`
  (`(require 'gogo-mode)` after adding the directory to `load-path`).

In any other editor, map the extension to Go: in VS Code without the
extension that is `"files.associations": { "*.gg": "go" }`, in Vim
`autocmd BufRead,BufNewFile *.gg setfiletype go`.

That is syntax only. `gopls` and `go vet` do not apply: the Go toolchain
does not read `.gg` files, and gogo is a subset of Go with its own
packages (`gogo/actor`, `gogo/net`, ...), which exist only inside the gogo
compiler. Programs are only ever compiled and run by `gogo`, never by `go
run`; `gogo build` is the judge of what is valid. Their sources are in
`lib/` of the gogo tree (one directory per package) if you want to read
an API.

## 2.5 Running on a board

Programs and firmware are separate on the board. The **firmware** (the VM,
drivers, Wi-Fi, file systems and the screen console) is flashed once, by
`gogo setup` (section 2.1) or by hand (section 2.6). Your **program
image** goes into its own flash partition and can be replaced in a
second, without touching the firmware.

### Build, flash and watch a program

```sh
gogo build docs/book/examples/02-hello -o hello.ggb
gogo flash hello.ggb
gogo monitor -reset -timeout 5 -until "gogo: vm exited"
```

- `gogo build` compiles the directory to an image file (`.ggb`; without
  `-o` it writes `<directory name>.ggb` in the current directory).
- `gogo flash` writes the image to the board's program partition. It needs
  `esptool` on your `PATH` (the one you installed for `gogo setup`, or
  ESP-IDF's, which its `export.sh` puts there) and uses
  the only `/dev/cu.usbmodem*` port it finds; pass `-port` if there are
  several. It also takes a directory, and builds it first:
  `gogo flash docs/book/examples/02-hello` does both steps.
- `gogo monitor` copies the board's console to your terminal. `-reset`
  restarts the board first, so you see the program from the beginning;
  `-timeout 5` stops after 5 seconds; `-until` stops at the first line containing
  that text.

The board prints a boot line, runs the program, and prints a line when the
VM exits:

```text
gogo <version>: image <N> bytes, arena <cap> bytes, free RAM <F> (largest <L>)
hello, gogo 42 true
gogo: vm exited (<code>), arena used <bytes>
```

Everything the program prints also appears on the board's screen; crash
reports (standard error) are shown in red.

> **Note.** Only one program can use the serial port at a time. Close
> `gogo monitor` (or any other serial terminal) before `gogo flash` or
> `gogo config`.

> **Note.** The image format has a version. When you update gogo and the
> version changes, reflash the firmware too (run `gogo setup` again, or
> section 2.6): an older firmware
> refuses a newer image with `gogo: bad image (unsupported version)`.

You now have the whole loop: edit, `gogo run` on the computer, then `gogo
build` and `gogo flash` for the board. The next chapter looks at the
language itself.

## 2.6 Flashing the firmware by hand

`gogo setup` is the easy way to flash the firmware. To build it yourself,
for example after changing the firmware, install ESP-IDF and use
`idf.py` from the gogo tree. gogo uses ESP-IDF v6.1.1:

```sh
mkdir -p ~/esp && cd ~/esp
git clone -b v6.1.1 --recursive --shallow-submodules --depth 1 https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32s3
```

Then, from the gogo tree, for the **M5StickS3**:

```sh
. ~/esp/esp-idf/export.sh
idf.py -C platform/esp32s3 build
idf.py -C platform/esp32s3 -p /dev/cu.usbmodemXXXX flash
```

The **Cardputer ADV** firmware is built in its own directory with its own
settings:

```sh
idf.py -C platform/esp32s3 -B platform/esp32s3/build-cardputer -DGOGO_BOARD=cardputer \
    -DSDKCONFIG=build-cardputer/sdkconfig \
    -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.cardputer" build
idf.py -C platform/esp32s3 -B platform/esp32s3/build-cardputer -p /dev/cu.usbmodemXXXX flash
```

Make sure you flash the right firmware to the right board (`esptool
chip-id` reports embedded PSRAM on the StickS3 and none on the Cardputer
ADV). Chapter 11 says more about the two boards.

`idf.py flash` writes the same three files as `gogo setup` and, like it,
leaves the NVS partition (and so the Wi-Fi settings) alone.
`gogo firmware` (chapter 12) builds a firmware with only the parts your
programs need.
