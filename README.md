# gogo

gogo is a small actor VM for microcontrollers and a compiler for a subset
of Go. You write programs in Go syntax as a set of isolated actors, in the
style of *Erlang* : each actor has its own heap and memory limit, they talk
only by copied messages, and supervisors restart the ones that crash.
Peripherals such as GPIO, the keyboard, files, sockets, the IMU, infrared
and audio are **ports**, which are actors that take commands and send
typed events.

The `gogo` tool compiles a program (a directory of `.gg` source files) to a
compact bytecode image (a `.ggb` file). The same
image runs on the board and on the host VM (macOS and Linux), so you can
develop and test on your computer first. The VM is plain C99 with no
third-party libraries. An idle actor costs about 150 bytes.

## Boards

| Board | Chip | Notes |
|---|---|---|
| M5StickS3 | ESP32-S3 (8 MB PSRAM) | buttons, battery, IMU, IR send and receive, mic and speaker, Wi-Fi |
| Cardputer ADV | ESP32-S3 (no PSRAM) | 56-key keyboard, `/sd` card, IMU, IR send, mic and speaker, Wi-Fi |

Both use one ESP-IDF firmware tree (`platform/esp32s3`), selected with
`GOGO_BOARD`. RP2040 boards are planned.

## Architecture

```
                           ┌────────────────────────────────────────────┐
  DEVELOPER HOST           │  gogo program  (*.gg, Go subset)           │
  (macOS / Linux)          │  actors · messages · ports · supervisors   │
                           └──────────────────────┬─────────────────────┘
                                                  │ import "gogo/..."
 ┌────────────────────────────────────────────────▼─────────────────────────┐
 │ lib/   standard library (.gg, embedded in the compiler)                  │
 │  system → VM builtins: actor port bytes fs sys net code kbd power imu ir │
 │                       audio screen gpio                                  │
 │  libraries in gogo:   supervisor strings strconv dhcp dns http sntp kv   │
 │                       ringlog wav sound irproto tui …                    │
 └────────────────────────────────────────────────┬─────────────────────────┘
 ┌────────────────────────────────────────────────▼─────────────────────────┐
 │ compiler/  (Go, module gogotools)                                        │
 │  load ─► go/types check ─► subset check ─► codegen ─► assembler ─► image │
 │  capability inference · //gogo:optional                                  │
 └────────────────────────────────────────────────┬─────────────────────────┘
 ┌──────────────────────────────┐                 │
 │ cmd/gogo  CLI                │       *.ggb  bytecode image
 │ build run dis flash monitor  │       (header · consts · code · types ·
 │ firmware config setup        │        requires · exports)
 └──────────────┬───────────────┘                 │
                │ esptool / USB serial            │
════════════════╪═════════════════════════════════╪══════════════════════════
  TARGETS       ▼                                 ▼
 ┌────────────────────────────────────────────────────────────────────────┐
 │ runtime/vm   VM CORE  (C99, ~41 KB, all state per Vm)                  │
 │ ┌──────────┐ ┌───────────┐ ┌─────────────┐ ┌──────────┐ ┌────────────┐ │
 │ │ image    │ │ interp +  │ │ actors      │ │ messages │ │ timers     │ │
 │ │ loader   │ │ scheduler │ │ heap + GC   │ │ copy     │ │ names      │ │
 │ │ requires │ │ step/dl   │ │ mem limits  │ │ quotas   │ │ links/mon  │ │
 │ └──────────┘ └───────────┘ └─────────────┘ └──────────┘ └────────────┘ │
 │ ┌───────────────────────┐ ┌───────────────────────────────────────────┐│
 │ │ modules (hot loading) │ │ port layer: drivers · event ring · sources││
 │ └───────────────────────┘ └───────────────────────────────────────────┘│
 └─────────────────────────────────────┬──────────────────────────────────┘
                                       │ PortDriver / PortSource
 ┌────────┬─────────┬─────────┬────────┴┬───────────┬──────────┬──────────┐
 │ net    │ net/tls │ vfs     │ stream  │ input     │ console  │ screen   │
 │ eth arp│ mbedTLS │ /flash  │ Chunk   │ kbd power │ text +   │ (tui)    │
 │ ip udp │ cert    │ /sd     │ Out Done│ imu ir    │ CJK font │ cells    │
 │ tcp    │ bundle  │ sandbox │ Closed  │ audio     │          │ LCD ANSI │
 └────────┴─────────┴─────────┴─────────┴───────────┴──────────┴──────────┘
                                       │ gogo_host + VmBoard tables
          ┌────────────────────────────┼─────────────────────────────┐
 ┌────────▼───────────────┐ ┌──────────▼───────────┐ ┌───────────────▼─────────┐
 │ platform/esp32s3       │ │ platform/unix        │ │ runtime/embed           │
 │ ESP-IDF · FreeRTOS     │ │ gogovm (host VM)     │ │ libgogo.a + gogo.h      │
 │ M5StickS3   Cardputer  │ │ sim net / gpio / mic │ │ gogo_step · gogo_post   │
 │ Wi-Fi LCD I2S IR IMU   │ │ --fs-root --without  │ │ host ports · sandbox fs │
 │ LittleFS FAT NVS PSRAM │ │                      │ │ → C, Rust, Go, Swift    │
 └────────────────────────┘ └──────────────────────┘ └─────────────────────────┘

```

Above the double line runs on your computer: programs import the
standard library, whose system packages compile to VM builtins and whose
other packages are ordinary gogo code; the compiler turns them into a
`.ggb` image. Below it runs on the target: the VM core schedules actors
and reaches every peripheral through ports, and the same core runs on
the boards, in the host VM, and inside other programs through
`libgogo.a`, each supplying its own `gogo_host` and `VmBoard` tables.

## Repository layout

| Path | What |
|---|---|
| `runtime/vm/` | the VM core: interpreter, actors, scheduler, GC, ports, modules, floats |
| `runtime/floatlib/` | `gogo/math` and `strconv.FormatFloat` / `ParseFloat` (pure C, no libm; built with the `GOGO_FLOAT` option) |
| `runtime/net/` | the TCP/IP stack (Ethernet, ARP, IPv4, ICMP, UDP, TCP), pure C; `runtime/net/tls/`, the TLS client port over it (mbedTLS) |
| `runtime/vfs/`, `runtime/stream/`, `runtime/input/`, `runtime/console/`, `runtime/screen/` | file systems, the stream protocol of every byte port (`Chunk`/`Out`/`Done`/`Closed`), input and audio ports, the screen console and fonts, the `screen` port (a text grid; `gogo/tui` builds on it) |
| `platform/unix/` | the host VM (`build/gogovm`) and its simulated hardware |
| `platform/esp32s3/` | the ESP-IDF firmware for both boards ([README](platform/esp32s3/README.md)) |
| `compiler/`, `cmd/gogo/` | the compiler and the `gogo` command (Go module `gogotools`, the repository root) |
| `lib/` | the standard library, `gogo/...`, as `.gg` sources embedded into the compiler ([package table](lib/README.md)) |
| `examples/` | demo programs, most of them for a board |
| `tests/` | golden output tests, network simulator tests, the memory footprint test |
| `docs/` | the book, the design document, known issues |

## Quick start

**On a board, in one command.** Install the `gogo` command (macOS and
Linux; on Windows, take `gogo-windows-<arch>.exe` from a
[release](https://github.com/c4pt0r/gogo/releases)) and esptool
(`pip install esptool` or `brew install esptool`), connect an M5StickS3
or Cardputer ADV over USB, and run `gogo setup`:

```sh
curl -fsSL https://raw.githubusercontent.com/c4pt0r/gogo/main/install.sh | sh
gogo setup
```

It identifies the board, downloads and checks the matching firmware,
flashes it (keeping the board's settings), sets up Wi-Fi and runs a first
program.

**From source, on your computer.** You need Go 1.27 or newer, a C99
compiler and `make`.

```sh
make vm                               # build/gogovm, the host VM
go build -o build/gogo ./cmd/gogo     # the gogo command
build/gogo run docs/book/examples/02-hello
```

Or without building the command first:

```sh
go run ./cmd/gogo build docs/book/examples/02-hello -o hello.ggb
go run ./cmd/gogo run hello.ggb
```

A program can be one file, or a script: `gogo run hello.gg` builds and
runs a single file, and a file starting with `#!/usr/bin/env -S gogo run`
runs by itself after `chmod +x`. A larger program is a directory with
packages of its own (`import "app/util"` is `app/util/`); packages shared
between programs go in `$GOGOPATH` (default `~/gogo`), and `gogo env`
prints the search path and the VM `gogo run` uses.

Without building from source, put `gogovm-<os>-<arch>` from the release
next to `gogo`, named `gogovm`, and `gogo run` works too (macOS and
Linux).

`gogo` with no arguments prints its commands: `build`, `run`, `env`,
`dis`, `info`, `flash`, `push`, `monitor`, `firmware`, `config`,
`device`, `setup` and `version`. Flags take one dash or two and may follow
the arguments (`gogo build app -o app.ggb`). `gogo run` exits with the
status of the program's `main` (0, 4 if it crashed, or its
`actor.Exit` code).

Once a board runs the firmware, flash programs on their own (`gogo flash`
builds a directory first), and copy modules to its `/flash` for hot
loading:

```sh
build/gogo flash docs/book/examples/02-hello
build/gogo monitor -reset -timeout 5
build/gogo push greeter.ggb               # to /flash/greeter.ggb, for code.Load
build/gogo device stats                   # the board's counters and memory
```

[Chapter 2 of the book](docs/book/02-getting-started.md) has the details,
including building and flashing the firmware by hand with ESP-IDF v6.1.1,
and editor setup for `.gg` files ([`editors/`](editors/README.md): VS Code, Vim and Neovim, Emacs). Releases are built by `tools/release.sh` (and the
release workflow on a `v*` tag).

Tests: `make test` runs the C tests (host and MCU configurations), the Go
tests (compiler, golden outputs, network simulator), the size checks,
`make lint` (gofmt, also on `.gg` files, go vet, and shellcheck when
installed), `make test-examples` (every example builds; those with an
expected output run) and every example in the book (`make test-book`,
`docs/book/examples/run.sh`). The test workflow
(`.github/workflows/test.yml`) runs it, `make test-lib` and
`make test-tls-asan` on macOS and Linux once a day and when started by
hand; it has passed on both.

## Embedding

gogo also runs inside other programs, the way Lua is embedded: `make lib`
builds `build/libgogo.a`, a C99 library with one header,
[`runtime/include/gogo.h`](runtime/include/gogo.h) (and
[`gogo_posix.h`](runtime/include/gogo_posix.h) for the POSIX host used
below). A host loads an image,
runs it (or steps it from its own event loop), posts messages to its
actors and gives it ports of its own; any number of VMs share one process.

```c
gogo_vm *vm;
if (gogo_new_posix(NULL, &vm) == GOGO_OK && gogo_load(vm, image, len, GOGO_LOAD_COPY) == GOGO_OK)
    gogo_run(vm);
gogo_free(vm);
```

Go programs use the binding `gogotools/embed` (build tag `libgogo`),
which can also compile source in process (`embed.Compile`); C programs
compile at run time with `libgogoc.a` (`make libgogoc`, `gogoc.h`).

The book chapter [Embedding gogo](docs/book/18-embedding.md) is the guide;
[`examples/embed-c/`](examples/embed-c/) and
[`examples/embed-go/`](examples/embed-go/) have the example hosts
(`make example-embed`).

## Documentation

- [Programming gogo](docs/book/README.md): the book, 19 chapters, every
  example tested. It reads as a website too: `make site` builds it with
  mdBook into `build/site`, next to the landing page.
- [docs/design.md](docs/design.md): the design document: the VM, the
  image format, the term encoding, ports, the network stack, storage and
  hot loading.
- [docs/bytecode.md](docs/bytecode.md): the bytecode reference: every
  opcode, the builtins, the reserved types and the image file layout,
  generated from `runtime/vm/opcodes.h`.
- [docs/known-issues.md](docs/known-issues.md): open limitations by area,
  with workarounds.
- [docs/FAQ.md](docs/FAQ.md): common questions: the language, the VM,
  ports, embedding and what is planned.

## Status and roadmap

Done:
- **Runtime:** actors, links, monitors and supervisors; per-actor memory
  limits; hot loading of modules; image format 15 (newer images run on
  older firmware unless they use something new, and an image names the
  drivers and mounts it needs).
- **Peripherals:** GPIO; keyboard and battery; IMU and infrared; audio;
  the screen as a port plus `gogo/tui`, an ncurses-like text UI; one
  stream protocol for every byte port.
- **Networking:** DHCP, DNS, UDP and TCP; HTTP client and server; HTTPS
  (a TLS 1.2 client, keep-alive client, streaming upload); SNTP and the
  wall clock.
- **Storage:** `/flash`, `/sd`, `kv`, `ringlog`.
- **Language and tools:** expression `switch`, labels, `range` over int,
  variadic functions; floating point (`float32`, `float64`, `gogo/math`);
  sized integers (`int8` .. `uint64`); methods, interfaces and `error`,
  `panic`; pointers, maps (in key order), closures that share what they
  capture, read-only package-level variables, `gogo/fmt`;
  packages of your own with a `GOGOPATH` search;
  single-file programs and `#!` scripts; `gogo setup`; optional firmware
  features; releases with the CLI, `gogovm` and firmware.
- **API v0.2:** shared error codes across the libraries and the C side,
  a versioned C ABI, CLI exit codes and commands (`push`, `info`,
  `device`), and a platform layer (HALs, one boot path) that makes new
  boards cheap.
- **Embedding:** `libgogo.a` with a C API (§5.11 of the design notes);
  a Go binding, and in-process compilation for Go and C hosts
  (`libgogoc.a`).

Next, in order:

1. **Distributed actors** (in progress): boards and a server node
   exchanging actor messages, first through the server; direct links
   between boards later.
2. **More hardware:** the Arduino Nano ESP32, RP2040 boards, Bluetooth
   (BLE); a `usb` port for the serial connection later.
3. **Embedding bindings:** Rust, iOS and Android.

## License

gogo is licensed under the BSD 3-Clause License, see [LICENSE](LICENSE).
Third-party components (the console font, the BMI270 configuration blob,
FatFs, LittleFS, and the ESP-IDF parts linked into the firmware) keep their
own licenses, listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
