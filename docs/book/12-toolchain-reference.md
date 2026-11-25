# 12. Toolchain Reference

Everything is done with one command, `gogo`, built with `go build -o
build/gogo ./cmd/gogo`. It runs on macOS and Linux.

```text
gogo build <dir> [-o out.gogo]
gogo run [--key c=pin]... [--fs-root DIR] <dir|file.gogo>
gogo dis <file.gogo>
gogo flash [-port P] [-esptool path] app.gogo
gogo monitor [-port P] [-t seconds] [-until text] [-reset]
gogo config [-port P | -local] set <key> <value|-> | get <key> | list | rm <key> | scan | stats
```

## gogo build

```text
gogo build <dir> [-o out.gogo]
```

Compiles the `package main` in `<dir>` (all its `.go` files except
`_test.go`) and the gogo libraries it imports into one image file. Without
`-o` the image is named after the directory, `<dir>.gogo`, in the current
directory. The image contains bytecode and constants and runs unchanged on
the host VM and on any board.

Errors are printed as `file:line:col: message` and the exit status is 1.
Messages from Go's type checker look like Go's (`declared and not used: x`);
messages about the subset start with `gogo:`:

```text
main.go:9:2: gogo: captured variable must not be reassigned
```

Type errors are reported first; subset errors appear once the program
type-checks. Chapter 3 lists the subset errors.

## gogo run

```text
gogo run [--key c=pin]... [--fs-root DIR] <dir|file.gogo>
```

Runs a program on the host VM. Given a directory it builds it first (to a
temporary image); given a `.gogo` file it runs that image. The exit status
is the VM's: 0 when the VM ran until nothing was left to do, 2 if the
image could not be loaded.

> **Note.** A crash, even of the `main` actor, does not change the exit
> status: crashes are reported on standard error and the VM goes on until
> it is idle. There is no way for a program to set the exit status, so
> scripts must look at the output to tell success from failure.

Options:

- `--fs-root DIR`: back `/flash` with `DIR/flash` (created if missing) and
  `/sd` with `DIR/sd` (only if it exists). Without it there is no storage.
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
`config.Get` reads (default `~/.gogo/config`).

There is no network on the host (chapter 10).

## gogo dis

```text
gogo dis <file.gogo>
```

Prints the bytecode of an image, function by function. Useful to see what
the compiler made of your code, or to compare two builds:

```text
$ gogo build docs/book/examples/02-hello -o hello.gogo
$ gogo dis hello.gogo
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
gogo flash [-port P] [-esptool path] app.gogo
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
board's network counters and free memory. Both need a board.

## Building the firmware

The firmware is an ESP-IDF project in `platform/esp32s3`, built with
`idf.py` (ESP-IDF v6.1.1). See chapter 2 for the commands and
`platform/esp32s3/README.md` for build options (`GOGO_BOARD`,
`GOGO_CONSOLE=0`, `CONFIG_GOGO_KBD=n`, `CONFIG_GOGO_BATTERY=n`) and
hardware details.

## Running the book's examples

```sh
make vm && go build -o build/gogo ./cmd/gogo
docs/book/examples/run.sh              # all examples
docs/book/examples/run.sh 04-call      # one
```

`run.sh` runs each example the way the book shows (with its `input`,
`fsroot` and `config` files) and compares the output with the
`expected.out` and `expected.err` files next to it.
