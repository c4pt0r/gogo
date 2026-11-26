# gogo

gogo is a small actor VM for microcontrollers and a compiler for a subset
of Go. You write programs in Go syntax as a set of isolated actors, in the
style of *Erlang* : each actor has its own heap and memory limit, they talk
only by copied messages, and supervisors restart the ones that crash.
Peripherals such as GPIO, the keyboard, files, sockets, the IMU, infrared
and audio are **ports**, which are actors that take commands and send
typed events.

The `gogo` tool compiles a program to a compact bytecode image. The same
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

## Repository layout

| Path | What |
|---|---|
| `runtime/vm/` | the VM core: interpreter, actors, scheduler, GC, ports, modules |
| `runtime/net/` | the TCP/IP stack (Ethernet, ARP, IPv4, ICMP, UDP, TCP), pure C; `runtime/net/tls/`, the TLS client port over it (mbedTLS) |
| `runtime/vfs/`, `runtime/stream/`, `runtime/input/`, `runtime/console/` | file systems, streams (`Chunk`/`Out`/`Done`), input and audio ports, the screen console and fonts |
| `platform/unix/` | the host VM (`build/gogovm`) and its simulated hardware |
| `platform/esp32s3/` | the ESP-IDF firmware for both boards ([README](platform/esp32s3/README.md)) |
| `compiler/`, `cmd/gogo/` | the compiler and the `gogo` command (Go module `gogotools`, the repository root) |
| `lib/` | the standard library, `gogo/...` (Go module `gogo`; [package table](lib/README.md)) |
| `examples/` | demo programs, most of them for a board |
| `tests/` | golden output tests, network simulator tests, the memory footprint test |
| `docs/` | the book, the design document, known issues |

## Quick start

You need Go 1.27 or newer, a C99 compiler and `make`.

```sh
make vm                               # build/gogovm, the host VM
go build -o build/gogo ./cmd/gogo     # the gogo command
build/gogo run docs/book/examples/02-hello
```

Or without building the command first:

```sh
go run ./cmd/gogo build docs/book/examples/02-hello -o hello.gogo
go run ./cmd/gogo run hello.gogo
```

`gogo` with no arguments prints its commands: `build`, `run`, `dis`,
`flash`, `monitor`, `firmware` and `config`.

To run on a board, build and flash the firmware once with ESP-IDF v6.1.1,
then flash programs on their own:

```sh
. ~/esp/esp-idf/export.sh
idf.py -C platform/esp32s3 build                       # M5StickS3 firmware
idf.py -C platform/esp32s3 -p /dev/cu.usbmodemXXXX flash
build/gogo build docs/book/examples/02-hello -o hello.gogo
build/gogo flash hello.gogo
build/gogo monitor -reset -t 5
```

[Chapter 2 of the book](docs/book/02-getting-started.md) has the details,
including the Cardputer ADV firmware build and editor support.

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
