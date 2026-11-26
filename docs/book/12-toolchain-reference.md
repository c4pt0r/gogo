# 12. Toolchain Reference

Everything is done with one command, `gogo`: prebuilt in each release
(`gogo-<os>-<arch>`, chapter 2), or built with `go build -o build/gogo
./cmd/gogo`. `gogo run` also needs the host VM, `gogovm` (released as
`gogovm-<os>-<arch>` for macOS and Linux, or built with `make vm`). It runs on macOS and Linux; on Windows, `setup` flashes the
firmware and programs, but the serial console commands (`monitor`,
`config` on a board, and the Wi-Fi and first-program steps of `setup`)
are not supported.

```text
gogo build [-path dirs] [-o out.ggb] <dir|file.gg>
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

**Flags** work the same in every command:

- A flag is spelt with one dash or two (`-port P`, `--port P`), and its
  value follows as the next argument or after `=` (`-o=app.ggb`).
- Flags may come before or after the other arguments: `gogo build app -o
  app.ggb` and `gogo build -o app.ggb app` are the same. `--` ends the
  flags, so an argument after it that starts with `-` is not one; a lone
  `-` is an ordinary argument. `gogo run` is the exception, as `go run`
  is: its flags come before the program, and what follows the program
  belongs to the program (refused for now).
- The names are the same everywhere: `-port` (the board's serial port),
  `-board`, `-o` (where the output goes), `-timeout` (seconds),
  `-esptool`, `-path`, `-yes`.
- `-h` (or `-help`) prints the command's usage and exits 0. A mistake
  (an unknown flag, a missing value or argument) prints what is wrong and
  the usage, and exits 1.

## gogo build

```text
gogo build [-path dirs] [-o out.ggb] <dir|file.gg>
```

Compiles the `package main` in `<dir>` (all its `.gg` files), the gogo
libraries and the packages of your own it imports into one image file.
Without `-o` the image is named after the directory, `<dir>.ggb`, in the
current directory. Given a `.gg` file instead (or a file starting with
`#!`), it compiles that file alone as `package main`, into `<name>.ggb`. The image contains bytecode and constants and runs unchanged on
the host VM and on any board.

Errors are printed as `file:line:col: message` and the exit status is 1
(every `gogo` command exits 1 on an error; `gogo run` has its own
statuses, below).
Messages from Go's type checker look like Go's (`declared and not used: x`);
messages about the subset start with `gogo:`:

```text
main.gg:9:2: gogo: captured variable must not be reassigned
```

Type errors are reported first; subset errors appear once the program
type-checks. Chapter 3 lists the subset errors.

### Packages and the search path

An import that does not start with `gogo/` names a package of your own
(section 3.1.1). `<dir>` (or the directory of the single file) is the
*build root*, and the import is looked up, first match wins:

1. in the build root, when its first element is the root directory's
   name: `import "app/util"` while building `app/` is `app/util/`;
2. in each directory of the search path, in order: `import "lib/ring"`
   is `<dir>/lib/ring/`.

A match is a directory holding `.gg` files. The search path is `-path
dirs` if given, else `$GOGOPATH` (directories separated by `:`, or `;`
on Windows; empty elements are skipped, relative ones are taken from the
current directory), else `$HOME/gogo` when `GOGOPATH` is unset. A
build root inside a search path directory counts once (the same directory
is one place). A path found in more than one place is a warning
naming every location; a path found nowhere is an error listing them:

```text
main.gg:3:8: gogo: import "app/nope": package not found; searched:
	app/nope (build root: no such directory)
	/home/you/gogo/app/nope (search path: no such directory)
```

A package's name must be its directory's name (so a Go identifier), only
the build root may be `package main`, an import path must match the
directory names on disk letter case included, and import cycles are
errors that show the chain. Functions of your packages are named by their
import path (`app/util.Sum`) in crash reports and `gogo dis`. A module
built for hot loading resolves its packages in the same way, from its own
directory.

Only `.gg` files are read: `.go` files in the directory are ignored, and a
directory with `.go` files but no `.gg` file is an error that says to
rename them (`no .gg files, only .go files: ...`). gogo sources were `.go`
files before; `.gg` is Go syntax, so map it to Go in your editor for
highlighting (chapter 2, *Editor support*); gopls does not apply.

### Requirements and `//gogo:optional`

The image also records, by name, what it needs from the board (its
requirements, appendix F): the driver of every `port.Open` with a constant
driver name (`"kbd"`, `"tcp"`, a host port's `"ui"`), and the mount `/sd`
for constant `/sd` paths, in `package main` and in every library compiled
in. An image that uses floating point (a float value, operation,
constant or type switch case, `gogo/math`, `strconv.FormatFloat` or
`ParseFloat`) also requires `float`, the firmware option `GOGO_FLOAT`. An
import by itself requires nothing. A firmware that lacks a required
driver or mount refuses the image at boot, naming it:

```text
gogo: image requires kbd, which this VM lacks
```

and `code.Load` returns `Unsupported` with `code.Missing` naming it
(chapter 14). A driver name the compiler cannot see (a variable, a
function call) is a loophole: it requires nothing, so it escapes this
check, and the open simply fails at run time on a board without the
driver. The build warns at each such call, with its position:

```text
gogo: warning: main.gg:12:9: port.Open with a non-constant driver name: the image does not require it and the load does not check it; name the driver and mark it //gogo:optional if the program handles its absence
```

To open a driver that may be absent, name it and mark it soft with
`//gogo:optional` (below), which works in `package main` too.

The image format itself has a version, `major.minor` (major 15; `gogo
version` prints the full one). A firmware runs images of its major whose
minor is at most its own, and the compiler stamps each image with the
lowest minor its program needs, so after a firmware update every module
already on `/flash` keeps loading. An image of another major, or of a newer minor than the
firmware's, is refused (`unsupported image format 15.2 (this VM runs 15.0
to 15.1)`; `code.Load` returns `BadVersion`). The minors so far: 15.1
added `screen.Config`, 15.2 floating point.

A package, a library or `package main`, can declare that it uses some
drivers only softly: it opens them only when a call asks for that at run
time, and handles their absence (`port.Open` returns `ok=false`) as an
ordinary failure. It says so with a directive line in any of its files,
conventionally the last line of the package doc comment, after an empty
`//` line (where gofmt puts directives):

```go
// Package http is a small HTTP/1.x client and server...
//
//gogo:optional tls
package http
```

- The listed drivers add no requirement **for that package's own**
  `port.Open` calls. The same driver opened by another package still
  counts: a program that calls `port.Open("tls", ...)` itself requires
  `tls`, unless `package main` names it in its own directive. A program
  that runs on several boards can so probe for a driver one of them lacks:

  ```go
  // Program keys uses the keyboard when the board has one.
  //
  //gogo:optional kbd
  package main
  ```

- Any driver name is accepted, a board's or a host port's. An empty list
  or a misspelt `//gogo:` directive is a compile error, for example
  `a.gg:3:1: gogo: unknown directive //gogo:optinal (want //gogo:optional)`.
- `gogo/http` declares `tls` (an http-only program runs on a firmware
  without TLS, where an https request fails), and `gogo/sound` declares
  `mic speaker` (`Record` returns false without a microphone, `Play` without a
  speaker).
- The directive is a promise the compiler does not check. A library can still hand `main` a function that opens the
  driver; that open then fails cleanly at run time (`ok=false`) on a
  firmware without the driver.
- `gogo firmware` (below) chooses features from the requirements, so it does not
  turn a soft driver on by itself; `tls`, `mic` and `speaker` are on by default,
  and `-without` makes those calls fail at run time instead.

## gogo run

```text
gogo run [--key c=pin]... [--fs-root DIR] [-path dirs] <dir|file.gg|file.ggb> [args...]
```

Runs a program on the host VM. Given a directory it builds it first (to a
temporary image), all of its `.gg` files together; given a `.gg` file, or
any file starting with `#!`, it builds that file alone, with its directory
as the build root for packages (an executable script with the first line
`#!/usr/bin/env -S gogo run` runs this way, chapter 2); given a `.ggb`
file it runs that image. `-path dirs` is the search path of the build, as
for `gogo build`.

**Program arguments.** Everything after the program is the program's, as
for `go run`: flags come before the program, and what follows it is passed
on untouched (`gogo run tool.gg -v "a b"` gives `tool.gg` the two arguments
`-v` and `a b`; `--` and empty words are passed too). The program reads
them with `sys.Args()`, a `[]string` of the arguments only: unlike Go's
`os.Args` there is no element 0 for the program's own name, so with no
arguments the slice is empty (and not `nil`). A `#!` script gets the
arguments of its own command line the same way (`./tool.gg -v` runs
`gogo run ./tool.gg -v`). `gogovm` takes them after the image,
`gogovm [flags] prog.ggb [args...]`. Every call to
`sys.Args()` copies the arguments into the actor's heap, so call it once
and keep the result; a very large command line (hundreds of KB) can run
the actor out of memory (a crash, `oom`, not an empty slice). On a board
nothing starts a program with arguments, so `sys.Args()` is empty there; a host
that embeds the VM can provide them (chapter 18).

**Exit status.** `gogo run` exits with the VM's status, which says how the
`main` actor ended:

| Status | Meaning |
|---|---|
| 0 | `main` exited `Normal` (or `Shutdown`) |
| 1 | a usage, toolchain or build error, or the VM was not found |
| 2 | the image could not be loaded: not an image, damaged, or a format this VM does not run |
| 3 | the image requires a driver or mount the VM lacks (for example `tls` in a VM built without mbedTLS) |
| 4 | `main` crashed: a VM reason such as `divzero`, `oom` or `killed` (appendix B), or a code below 64 or negative given to `actor.Exit` |
| 64..99 | `main` exited with a library failure, `actor.Failed` plus a shared code (appendix B), as it is: `http.ListenFailed` is 69 |
| 100..255 | `main` exited with your code, `actor.Exit(code)` with `code` from 100; a code above 255 is 255 |

Only `main` counts: other actors' crashes are reported on standard
error but leave the status alone, so a program whose `main` ends
normally exits 0. To fail a script, end `main` with a code:

```go
if !ok {
	println("check failed")
	actor.Exit(100) // gogo run exits 100
}
```

When a linked actor's exit takes `main` down, `main` exits with that
actor's own reason: 4 if it crashed, but 150 if it called
`actor.Exit(150)`, 69 for a library failure.

If the VM itself is killed by a signal, `gogo run` exits 128 plus the
signal's number, as a shell does (130 for Ctrl-C, 137 for `kill -9`).
Those statuses overlap the user range 100..255, so a script that must
tell them apart should keep its own codes below 128. The host VM
`build/gogovm` exits with the same statuses (it is where they come
from).

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
  (used, quota). See section 16.5. And it takes `--without NAME,...`
  (tests only): the named drivers and mounts (`kbd`, `tls`, `/sd`, ...)
  are left out of the host board, as on a firmware built without them.
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

**The VM.** `gogo run` uses the first of: `$GOGO_VM`; `gogovm` (or the
release asset name `gogovm-<os>-<arch>`) in the directory of the `gogo`
executable, symlinks resolved; `build/gogovm` in the gogo source tree
around the `gogo` executable or the current directory. If none is found
it says so and what to do. `gogo env` prints the one it would use.

**Environment.** `GOGO_VM` names the VM to use (above). `GOGOPATH` is the
package search path of the build. `GOGO_CONFIG` names the configuration file that
`sys.Config` reads (default `~/.gogo/config`). `GOGO_UNIX_TIME_MS` starts
the wall clock (`sys.Time`, chapter 10.8) at that Unix time in ms instead of
the computer's clock; `0`, or a time earlier than the VM's build, starts it
unknown, as on a board.

There is no network on the host (chapter 10).

## gogo env

```text
gogo env [-path dirs]
```

Prints the version, the package search path (where it comes from:
`$GOGOPATH`, `$HOME/gogo` when `GOGOPATH` is unset, or `-path`) and the VM
`gogo run` would use, with where it was found:

```text
$ gogo env
version   v0.4.0 (image format 15.2)
GOGOPATH  /home/you/gogo ($HOME/gogo, GOGOPATH is unset)
search    1. the build root (imports starting with its directory's name)
          2. /home/you/gogo
gogovm    /usr/local/bin/gogovm (next to gogo)
```

A VM that cannot be found is reported on the `gogovm` line, and the exit
status is still 0.

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

## gogo info

```text
gogo info [-path dirs] <file.ggb|dir|file.gg>
```

Says what an image is and what it needs, read from the image file: its
format version (`major.minor`, the lowest the program needs, so the
oldest firmware that runs it), its size, the drivers and mounts it
requires (what a firmware must have to boot it, or to `code.Load` it as
a module), and its entries (the exported functions `code.Spawn` starts by
name; `(boot)` marks the one a board starts):

```text
$ gogo info examples/netdemo
format     15.0
size       46112 bytes: 94 functions, 58 types, 9605 code words, 1462 constant words
requires   gpio, netif, tcp, tcp_listen, udp
optional   tls (gogo/http)
entries    main (boot)
```

Given a directory or a `.gg` file, it builds it in memory first (`-path`
as for `gogo build`) and adds the `optional` line: the drivers the
program's packages mark `//gogo:optional` (with the packages that do),
which the image does not record because a firmware without them runs it
anyway. A file that is not a gogo image, is damaged, or has a format
this `gogo` does not know is an error.

## gogo flash

```text
gogo flash [-port P] [-esptool path] [-path dirs] <app.ggb|dir|file.gg>
```

Writes an image to the board's program partition at 0x210000, with a
header and a CRC that the firmware checks at boot. Given a directory or a
`.gg` file it builds it first, as `gogo run` does (to a temporary image;
`-path` as for `gogo build`), so `gogo flash ./app` is `gogo build` and
`gogo flash` in one step. Needs the board's gogo firmware to be installed
already (chapter 2).

- `-port P`: the serial port. Default: the only `/dev/cu.usbmodem*`; with
  none or several, `gogo flash` stops and asks for `-port`.
- `-esptool path`: the esptool to run. Default: `esptool` or `esptool.py`
  from `PATH` (`. ~/esp/esp-idf/export.sh` puts it there).

`gogo flash` refuses files that are not gogo images, images of another
format version (`rebuild it with gogo build`) and images over the 1 MB
partition.

## gogo push

```text
gogo push [-port P] [-timeout seconds] <file.ggb> [/flash/path]
```

Copies an image (a module, chapter 14) to the board's `/flash`, over the
serial console while the board runs: to `/flash/<file name>`, or to the
path given (its directory must exist; names as in chapter 9). The board
writes it under a temporary name, reads it back, checks its length and
CRC-32, and only then renames it over the target, so the target is
either the old file or the whole new one. Any failure, a refused chunk,
a mismatch, the cable pulled, leaves the target as it was. The temporary
file is `.gogo-push` in the target's directory (which is why that name
cannot be a target); a push cut off by a pulled cable leaves it there,
taking space and listed by `fs.ReadDir`, until the next push into that
directory.

- `-port P`: as for `gogo flash`.
- `-timeout seconds`: how long to wait for each answer of the board
  (default 5).

It refuses a file that is not an image of a format this `gogo` knows,
and a path outside `/flash`. A firmware older than `gogo push` answers
that it has no push. Only one program can have the serial port open:
close `gogo monitor` first.

## gogo monitor

```text
gogo monitor [-port P] [-timeout seconds] [-until text] [-reset]
```

Copies the board's console to standard output.

- `-timeout seconds`: stop after this long (default 10; fractions such
  as `0.5` are fine). `-t` is its old name: it still works in this
  release, with a note on standard error, and goes away in the next.
- `-until text`: stop after the first line containing `text`, e.g.
  `-until "gogo: vm exited"`.
- `-reset`: reset the board first, so the capture starts at boot. Without
  it, opening the port never resets the board.
- `-port P`: as for `gogo flash`.

It is not interactive. Only one program can have the serial port open, so
stop `gogo monitor` before `gogo flash` or `gogo config`.

## gogo firmware

```text
gogo firmware (-board sticks3|cardputer | -port P [-esptool path]) [-o dir] [-build] [-with list] [-without list] files...
```

Chooses which optional parts the firmware needs to contain for a set of
images and modules, so a build does not carry a network stack or an SD
driver nobody uses. It reads the requirement names each file carries
(what `gogo build` writes), maps each to the feature that builds it, and writes
`<dir>/sdkconfig.gogo`, a list of `CONFIG_GOGO_*` lines, then prints the
`idf.py` command that builds with it. It never flashes.

- Feature names: `net kbd power sd exfat cjk imu irtx irrx mic speaker tls screen float`.
  The requirement names they build: `net` the drivers `netif`, `udp`,
  `tcp` and `tcp_listen`; `kbd`, `power`, `imu`, `mic`, `speaker`, `tls` and
  `screen` the driver of the same name; `irtx` and `irrx` the drivers `ir_tx` and
  `ir_rx`; `sd` the mount `/sd` (appendix F); `float` the requirement
  `float` (floating point in the VM and `gogo/math`). `gpio` and `/flash`
  are in every firmware; any other name (a host port's, say) is an error.
  `net`, `kbd`, `power` and `float` are on only when a file requires them (or
  `-with` adds them). The others are on by default on boards that have
  them: `sd`, `imu`, `irtx`/`irrx` (the infrared sender and receiver),
  `mic`/`speaker` and `tls` (the TLS client, chapter 17) can be required too,
  so a file that needs one keeps it on, while `exfat` (exFAT media) and
  `cjk` (the Chinese font) are not. `irtx` and `irrx` share one firmware switch, as do `mic` and
  `speaker`, so they go on and off together.
- `-with a,b` adds features. `-without a,b` removes them; a feature that a
  file requires cannot be removed (error). `exfat` goes off with `sd`, and
  `tls` with `net`.
- A required feature the board lacks is an error that names it, for example
  `kbd` or `sd` on the `sticks3`.
- `-board` names the board. Instead, `-port P` asks the board on that
  port: one read-only esptool command (`chip-id`; `-esptool` as for
  `gogo flash`) reads its chip, and an ESP32-S3 with embedded PSRAM is
  the M5StickS3, one without is the Cardputer ADV (as `gogo setup`
  decides). The board resets afterwards and runs its program again.
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
gogo config keys
gogo config [-port P] scan        # deprecated: gogo device scan
gogo config [-port P] stats       # deprecated: gogo device stats
```

Edits the configuration store on the board or in the local file. See
chapter 11 for how the store is chosen, the key rules and the keys gogo
uses (`keys` prints them; `set` checks a key under a gogo prefix, and its
value, against that table). `scan` and `stats` are `gogo device scan` and
`gogo device stats` (below), kept here for this release.

## gogo device

```text
gogo device [-port P] scan
gogo device [-port P] stats
gogo device [-port P] [-yes] format-flash
```

Operations on the board itself, over its serial console:

- `scan` lists the 2.4 GHz networks the board sees.
- `stats` prints the board's network counters, free memory and how full
  the VM's fixed tables are, as used/capacity: `bytes=N/64` (off-heap
  `bytes.Bytes` handles), `ports=N/16` (open ports) and `subs=N/4`
  (`net.Subscribe` slots). When a port open fails or a tcp write gets
  `Done{-1}`, these show whether a table is full. A firmware built
  without `net` prints no network counters and no `subs`.
  `console_stack_free=N/4096` is the least stack the console task (which
  answers these commands and writes `gogo push` files) has had free.
- `format-flash` erases every file on `/flash`: the board restarts and
  formats its `/flash` partition at the size `fs.flash.size` sets
  (chapter 9). Nothing else is touched: not the configuration store
  (Wi-Fi settings stay), not the program, not the firmware; the chip is
  never erased. It asks you to type `yes` first; `-yes` skips the
  question.

`-port` as for `gogo flash`. `scan` and `stats` were `gogo config scan`
and `gogo config stats`, and `format-flash` was `gogo config set
fs.flash.format yes` and a restart; the old forms still work in this
release, with a note, and go away in the next.

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

A release's version is also the C embedding API's: the tag's `X.Y.Z`
(`v0.2.0`, or `v0.2.0-rc1`) must equal `GOGO_VERSION_MAJOR`, `_MINOR` and
`_PATCH` in `runtime/include/gogo.h` (what `gogo_version()` reports, book
chapter 18), and `tools/release.sh` refuses the tag otherwise. So every
release bumps them, a firmware-only patch release included.

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

`make test-book` does the same. `run.sh` runs each example the way the book shows (with its `input`,
`fsroot` and `config` files) and compares the output with the
`expected.out` and `expected.err` files next to it. An example of several
packages keeps its program in `app/` (the build root, so its imports
start with `app/`); `run.sh` runs it from there, with an empty search
path.

## Checking a change to gogo

`make test` is the full check before a commit. Besides the C and Go
tests, it runs:

- `make lint`: `gofmt` on every `.go` file and on every `.gg` file
  (gogo source is Go syntax, but `gofmt` reads only `.go`, so
  `tools/lint.sh` checks a `.go` copy of each), `go vet ./...`, and
  `shellcheck` on `tools/*.sh` when it is installed. To format a `.gg`
  file, format a `.go` copy (or use `gofmt` on standard input:
  `gofmt < f.gg > f.tmp && mv f.tmp f.gg`).
- `make test-examples`: every program in `examples/` and
  `platform/esp32s3/examples/` must build (the board-only ones too:
  building needs no board), and the ones with an `expected.out` run with
  `gogo run` and must print it (`tools/examples.sh`, which reads the
  same `input`, `config`, `fsroot`, `mods/` and `expected.err` files as
  the golden tests). A new example without an `expected.out` is still
  built on every run. An example of several packages keeps its program
  in `app/` (with `app/expected.out`); a `.gg` file beside `app/` is a
  script, run when a `<name>.expected.out` is beside it; and a file
  `gogopath` names the example's `GOGOPATH`, relative to the example
  (`examples/packages` uses all three).
- `make test-book`: every example in this book, through
  `docs/book/examples/run.sh` (above).
- `make vmsize` (and `netsize`, `vfssize`, `screensize`): the code size
  of the VM core and of each runtime part for the ESP32-S3, against
  their budgets (skipped without the ESP-IDF compiler). `vmsize` also
  prints the VM core's lines of C; `tools/vmsize.sh --record` appends
  the size and the line count to `docs/vmsize-history.csv`, and
  `tools/vmsize.sh --no-float` measures the core without floating point.

Outside `make test`: `make test-lib` (the standard library on the host
Go toolchain, also part of `go test ./...`) and `make test-tls-asan`
(the network and TLS tests under AddressSanitizer). The test workflow,
`.github/workflows/test.yml`, runs all three on macOS and Linux
(x86-64) once a day and when started by hand (Actions > test > Run
workflow). The TLS tests need mbedTLS's
source (ESP-IDF's `components/mbedtls/mbedtls`; `MBEDTLS_DIR` names
another copy) and are skipped without it; the workflow fetches the
commit ESP-IDF v6.1.1 pins.
