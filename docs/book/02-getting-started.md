# 2. Getting Started

This chapter builds the tools, runs a first program on your computer, and
then puts it on a board.

## 2.1 What you need

On your computer (macOS or Linux):

- **Go 1.27 or newer**, to build the `gogo` command (the compiler and
  board tools). It uses only Go's standard library.
- **A C compiler and `make`**, to build `gogovm`, the VM that runs
  programs on your computer. Any C99 compiler works (`cc`, `clang`, `gcc`).
- **The gogo source tree.**

For a board you also need ESP-IDF, covered in section 2.5. You do not need
it to follow most of this book: every example runs on your computer.

## 2.2 Building the tools

From the root of the gogo source tree:

```sh
make vm                          # builds build/gogovm, the host VM
go build -o build/gogo ./cmd/gogo
```

`build/gogo` is the only command you use directly. When it runs a program it
looks for the VM at `build/gogovm` in the gogo tree it was built from (or in
the tree around your current directory); set `GOGO_VM=/path/to/gogovm` to
use another one.

Run it without arguments to see what it can do:

```text
$ build/gogo
usage: gogo build <dir> [-o out.gogo]
       gogo run [--key c=pin]... [--fs-root DIR] <dir|file.gogo>
       gogo dis <file.gogo>
       gogo flash [-port P] [-esptool path] app.gogo
       gogo monitor [-port P] [-t seconds] [-until text] [-reset]
       gogo config [-port P | -local] set <key> <value|-> | get <key> | list | rm <key> | scan | stats
```

The rest of this book writes `gogo` for `build/gogo`. Put it on your `PATH`
if you like.

## 2.3 Your first program

A gogo program is a directory holding one `package main` (in one or more
`.go` files) with a `func main()`:

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
pending, no input can arrive) the VM exits. Exit status 0 means the VM ran
to the end, not that every actor succeeded; crashes are reported on
standard error, as you will see in chapter 5.

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

The gogo packages (`gogo/actor`, `gogo/port`, ...) exist as ordinary Go
source in the gogo tree (function bodies are just `panic("gogo only")`), so
an editor with `gopls` can complete and type-check your program when its
directory is part of the gogo module (a directory under the tree root
without its own `go.mod`). Programs are only ever compiled and run by
`gogo`, never by `go run`.

> **Note.** This works for most packages but not for the network ones:
> `go vet` on such a directory accepts a program using `gogo/actor`, but one
> importing `gogo/net` fails with `package gogo/net is not in std`, and one
> importing `gogo/dhcp`, `gogo/dns` or `gogo/http` fails with `build
> constraints exclude all Go files`. Expect red squiggles under network
> code in your editor; `gogo build` is the real judge. (The book's own
> examples live under a nested `go.mod` to keep them away from `go vet`.)

## 2.5 Running on a board

Programs and firmware are separate on the board. The **firmware** (the VM,
drivers, Wi-Fi, file systems and the screen console) is built with ESP-IDF
and flashed once. Your **program image** goes into its own flash partition
and can be replaced in a second, without rebuilding the firmware.

### Install ESP-IDF and flash the firmware (once)

gogo uses ESP-IDF v6.1.1:

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

### Build, flash and watch a program

```sh
gogo build docs/book/examples/02-hello -o hello.gogo
gogo flash hello.gogo
gogo monitor -reset -t 5 -until "gogo: vm exited"
```

- `gogo build` compiles the directory to an image file.
- `gogo flash` writes the image to the board's program partition. It needs
  `esptool` on your `PATH` (the ESP-IDF `export.sh` puts it there) and uses
  the only `/dev/cu.usbmodem*` port it finds; pass `-port` if there are
  several.
- `gogo monitor` copies the board's console to your terminal. `-reset`
  restarts the board first, so you see the program from the beginning;
  `-t 5` stops after 5 seconds; `-until` stops at the first line containing
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

> **Note.** The image format has a version. When you update the gogo tree
> and the version changes, reflash the firmware too: an older firmware
> refuses a newer image with `gogo: bad image (unsupported version)`.

You now have the whole loop: edit, `gogo run` on the computer, then `gogo
build` and `gogo flash` for the board. The next chapter looks at the
language itself.
