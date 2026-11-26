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
 │  stubs → VM builtins: actor port bytes fs sys net code kbd power imu ir  │
 │                       audio                                              │
 │  libraries in gogo:   supervisor strings strconv dhcp dns http sntp kv   │
 │                       ringlog wav sound irproto …                        │
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
 │ runtime/vm   VM CORE  (C99, ~37 KB, all state per Vm)                  │
 │ ┌──────────┐ ┌───────────┐ ┌─────────────┐ ┌──────────┐ ┌────────────┐ │
 │ │ image    │ │ interp +  │ │ actors      │ │ messages │ │ timers     │ │
 │ │ loader   │ │ scheduler │ │ heap + GC   │ │ copy     │ │ names      │ │
 │ │ caps     │ │ step/dl   │ │ mem limits  │ │ quotas   │ │ links/mon  │ │
 │ └──────────┘ └───────────┘ └─────────────┘ └──────────┘ └────────────┘ │
 │ ┌───────────────────────┐ ┌───────────────────────────────────────────┐│
 │ │ modules (hot loading) │ │ port layer: drivers · event ring · sources││
 │ └───────────────────────┘ └───────────────────────────────────────────┘│
 └─────────────────────────────────────┬──────────────────────────────────┘
                                       │ PortDriver / PortSource
 ┌──────────┬──────────┬───────────┬───┴──────┬──────────────┬────────────┐
 │ net      │ net/tls  │ vfs       │ stream   │ input        │ console    │
 │ eth arp  │ mbedTLS  │ /flash    │ Chunk    │ kbd power    │ text +     │
 │ ip udp   │ cert     │ /sd       │ Out      │ imu ir       │ CJK font   │
 │ tcp      │ bundle   │ sandbox   │ Done     │ mic spk      │            │
 └──────────┴──────────┴───────────┴──────────┴──────────────┴────────────┘
                                       │ gogo_host + VmBoard tables
          ┌────────────────────────────┼─────────────────────────────┐
 ┌────────▼───────────────┐ ┌──────────▼───────────┐ ┌───────────────▼─────────┐
 │ platform/esp32s3       │ │ platform/unix        │ │ runtime/embed           │
 │ ESP-IDF · FreeRTOS     │ │ gogovm (host VM)     │ │ libgogo.a + gogo.h      │
 │ M5StickS3   Cardputer  │ │ sim net / gpio / mic │ │ gogo_step · gogo_post   │
 │ Wi-Fi LCD I2S IR IMU   │ │ --fs-root --caps     │ │ host ports · sandbox fs │
 │ LittleFS FAT NVS PSRAM │ │                      │ │ → C, Rust, Go, Swift    │
 └────────────────────────┘ └──────────────────────┘ └─────────────────────────┘

 Release:  git tag vX.Y.Z ─► GitHub Actions ─► draft release
           (firmware per board · gogo CLI per OS · SHA256SUMS · licenses)
           └─► gogo setup: detect board → verify → flash → Wi-Fi → hello
```

Above the double line runs on your computer: programs import the
standard library, whose stub packages compile to VM builtins and whose
other packages are ordinary gogo code; the compiler turns them into a
`.ggb` image. Below it runs on the target: the VM core schedules actors
and reaches every peripheral through ports, and the same core runs on
the boards, in the host VM, and inside other programs through
`libgogo.a`, each supplying its own `gogo_host` and `VmBoard` tables.

## Repository layout

| Path | What |
|---|---|
| `runtime/vm/` | the VM core: interpreter, actors, scheduler, GC, ports, modules |
| `runtime/net/` | the TCP/IP stack (Ethernet, ARP, IPv4, ICMP, UDP, TCP), pure C; `runtime/net/tls/`, the TLS client port over it (mbedTLS) |
| `runtime/vfs/`, `runtime/stream/`, `runtime/input/`, `runtime/console/` | file systems, streams (`Chunk`/`Out`/`Done`), input and audio ports, the screen console and fonts |
| `platform/unix/` | the host VM (`build/gogovm`) and its simulated hardware |
| `platform/esp32s3/` | the ESP-IDF firmware for both boards ([README](platform/esp32s3/README.md)) |
| `compiler/`, `cmd/gogo/` | the compiler and the `gogo` command (Go module `gogotools`, the repository root) |
| `lib/` | the standard library, `gogo/...`, as `.gg` sources embedded into the compiler ([package table](lib/README.md)) |
| `examples/` | demo programs, most of them for a board |
| `tests/` | golden output tests, network simulator tests, the memory footprint test |
| `docs/` | the book, the design document, known issues |

## Quick start

**On a board, in one command.** Install the `gogo` command from a
[release](https://github.com/c4pt0r/gogo/releases) (`gogo-<os>-<arch>`)
and esptool (`pip install esptool` or `brew install esptool`), connect an
M5StickS3 or Cardputer ADV over USB, and run:

```sh
gogo setup
```

It identifies the board, downloads and checks the matching firmware,
flashes it (keeping the board's settings), sets up Wi-Fi and runs a first
program. While the repository is private, the release download needs a
GitHub login: fetch a release with `gh release download -R c4pt0r/gogo
-D gogo-release` and run `gogo setup -release-url gogo-release`.

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

`gogo` with no arguments prints its commands: `build`, `run`, `dis`,
`flash`, `monitor`, `firmware`, `config`, `setup` and `version`.

Once a board runs the firmware, flash programs on their own:

```sh
build/gogo build docs/book/examples/02-hello -o hello.ggb
build/gogo flash hello.ggb
build/gogo monitor -reset -t 5
```

[Chapter 2 of the book](docs/book/02-getting-started.md) has the details,
including building and flashing the firmware by hand with ESP-IDF v6.1.1,
and editor setup for `.gg` files. Releases are built by `tools/release.sh` (and the
release workflow on a `v*` tag).

Tests: `make test` runs the C tests (host and MCU configurations), the Go
tests (compiler, golden outputs, network simulator) and the size checks.
`docs/book/examples/run.sh` checks every example in the book.

## Embedding

gogo also runs inside other programs, the way Lua is embedded: `make lib`
builds `build/libgogo.a`, a C99 library with one header,
[`runtime/include/gogo.h`](runtime/include/gogo.h). A host loads an image,
runs it (or steps it from its own event loop), posts messages to its
actors and gives it ports of its own; any number of VMs share one process.

```c
gogo_vm *vm = gogo_new_posix(NULL);
gogo_load(vm, image, len, GOGO_LOAD_COPY);
gogo_run(vm);
gogo_free(vm);
```

The book chapter [Embedding gogo](docs/book/18-embedding.md) is the guide;
[`examples/embed-c/`](examples/embed-c/) has the example hosts
(`make example-embed`).

## Documentation

- [Programming gogo](docs/book/README.md): the book, 18 chapters, every
  example tested.
- [docs/design.md](docs/design.md): the design document: the VM, the
  image format, the term encoding, ports, the network stack, storage and
  hot loading.
- [docs/known-issues.md](docs/known-issues.md): open limitations by area,
  with workarounds.

## Status and roadmap

Done: actors, links, monitors and supervisors; GPIO; strings, bytes and
slices; networking (DHCP, DNS, UDP, TCP, HTTP/1.0 client and server);
storage (`/flash`, `/sd`, `kv`, `ringlog`); keyboard and battery; hot
loading of modules; optional firmware features; IMU and infrared; audio;
the wall clock (`gogo/sys`) and SNTP; the repository layout; HTTPS, a
TLS 1.2 client (the `"tls"` port, mbedTLS, sliced so actors keep
running during a handshake), `https://` URLs in `gogo/http`, the
keep-alive `http.Client` and streaming file upload ([book chapter 17](docs/book/17-https.md)); image v13.

Next, in order:

1. **Embed mode** (in progress, phase 1 of 5): embedding the VM in host
   programs the way Lua is embedded.
2. **Language completeness**: more of Go (methods, maps, arrays, type
   assertions, the expression `switch`, ...).
3. **Bluetooth (BLE)**: a phone app connection and Wi-Fi setup from the
   phone.

## License

gogo is licensed under the BSD 3-Clause License, see [LICENSE](LICENSE).
Third-party components (the console font, the BMI270 configuration blob,
FatFs, LittleFS, and the ESP-IDF parts linked into the firmware) keep their
own licenses, listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
