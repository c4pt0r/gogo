# gogo FAQ

## Language and toolchain

### Is gogo Go?
No. gogo programs are written in a subset of Go syntax and checked by Go's
own type checker, but they compile to gogo bytecode for gogo's actor VM, not
to native code, and many Go features are not supported (or not yet). That is
why source files end in `.gg` and compiled images in `.ggb`. See
[book chapter 3](book/03-language-basics.md) for the subset and §3.10 for
what is missing.

### What is gogo for?
Small connected devices that do several things at once and must keep
running: a handheld or sensor node that samples an IMU, records audio to
SD, talks to a server over Wi-Fi/TLS and drives a small screen, all as
separate actors, so one failing part crashes alone and its supervisor
restarts it while the rest keeps going. Typical uses:

- **Connected gadgets and sensor nodes** (the M5StickS3, the Cardputer ADV):
  logging, recorders, remotes (IR), dashboards and tools with a text UI.
- **Devices that are updated in the field**: hot loading replaces modules
  without reflashing firmware or losing the rest of the running program.
- **Fast iteration on hardware**: write Go-like code, test it on your
  computer with simulated hardware (`gogovm`), then run the same image on
  the board.
- **Scripting inside other programs**: embed the VM (`libgogo.a`) in a
  desktop, server or mobile app to run untrusted or user-written logic in a
  sandbox (memory limits, capabilities, a jailed `/flash`).
- **Device-to-server systems** (planned): actors on boards and on a server
  sending each other messages directly.

What it is not for: hard real-time work with sub-millisecond deadlines
(code is interpreted, actors have garbage-collected heaps and a TLS
handshake can pause everything for about 200 ms), heavy number crunching or
signal processing (an interpreter, and no floats yet; do such work in C drivers),
and the very smallest microcontrollers (the VM core alone is about 37 KB of
code and needs tens of KB of RAM).

### Why actors?
Because a small device is a bundle of things happening at once (button
presses, sensor samples, network packets, audio buffers, timers), and the
usual ways of handling that each have a sharp edge: callbacks tangle state,
RTOS tasks sharing memory need locks and fail by corrupting each other.
Actors keep each activity in its own lightweight process (about 150 bytes
idle) with **its own heap**, talking only by copied messages:

- **Isolation.** A bug or out-of-memory in one actor crashes only that
  actor; no other actor's data can be touched. Each actor can be given a
  memory limit.
- **Supervision.** A supervisor restarts what crashed ("let it crash"), so
  a device can recover from faults instead of rebooting.
- **No locks.** Nothing is shared, so there are no data races or deadlocks
  on shared state.
- **Small GC pauses.** Each heap is collected on its own, so a collection
  touches one actor's data, not the whole program's.
- **One model for everything.** Hardware is ports, which are actors too:
  an interrupt, a key or a TCP packet arrives as an ordinary message in the
  same receive loop. Hot loading swaps actors' code, and the planned
  distribution sends the same messages across devices.

This is the model Erlang has used for decades in systems that must not go
down, scaled down to a microcontroller.

### Why a subset of Go?
Go's syntax is small, familiar and statically typed, so most mistakes are
found when you build, not on the device. Reusing it also gives gogo a
proven front end for free (Go's own parser and type checker), editor syntax
highlighting and a language many people already read.

It is a subset because some of Go conflicts with the design or does not fit
the device: goroutines and channels share memory between threads, which
would break actor isolation (actors and messages replace them); reflection
and a full runtime would not fit the memory budget. What remains is
the part that maps cleanly onto isolated actors and a small VM. The subset
grows where that holds (methods, interfaces, maps, floats and more are planned; see
above), and where gogo differs from Go it says so.

Dynamic languages (Lua, MicroPython) find type errors only at run time and
are harder to keep small and isolated per task; Rust or C give no isolation
between tasks, no hot loading and a slower edit-test loop on hardware.

### Why is gogo so small?
The VM core is about 37 KB of code, an idle actor about 150 bytes, and a
"hello" image about 1 KB, because size was a constraint from the first
line, not an afterthought:

- **The compiler stays on your computer.** Parsing, type checking and code
  generation never run on the device, so the firmware carries no compiler,
  parser or type tables, and type questions are settled before the code
  ships.
- **A simple, fixed bytecode.** 32-bit instruction words for a register
  machine, decoded by one `switch` loop: no JIT, no dispatch tables
  generated at run time.
- **Values are compact.** Small integers live directly in a tagged word, so
  they need no heap object, and struct values are immutable and shared
  inside an actor instead of copied.
- **Images run in place.** Code and constants are read straight from flash;
  nothing is copied into RAM to start a program.
- **Small heaps, simple GC.** Each actor has its own small heap collected by
  a copying collector, which is little code and needs no global
  bookkeeping.
- **The library is mostly gogo, not C.** Only the stub packages map to VM
  builtins; HTTP, DNS, supervisors, WAV files, the text UI and the rest are
  ordinary gogo code that lives in the program image, so a program only
  pays for what it imports.
- **Everything optional is optional.** Networking, TLS, SD/exFAT, the CJK
  font, audio, IR, the IMU and the screen are compile-time switches; a
  firmware without them is simply smaller.
- **A budget that is enforced.** Every change records the VM core size
  against a 64 KB limit, new mutable globals fail the build, and features
  that would cost too much (a JIT, a hash map in C) are designed out.

The whole firmware image is about 1 MB, but most of that is ESP-IDF itself
(Wi-Fi, the TCP/TLS libraries, the font): the gogo VM is a small part of it.

### How is gogo different from MicroPython?
Both run a high-level language on a microcontroller through a bytecode VM,
but they make opposite choices in several places:

| | gogo | MicroPython |
|---|---|---|
| Language | A subset of Go, statically typed | A subset of Python, dynamically typed |
| Where it compiles | On your computer; the device only runs `.ggb` images | On the device (the compiler is in the firmware), which also gives a REPL |
| Type errors | Found by `gogo build`, before the code ships | Found when that line runs |
| Concurrency | Actors with separate heaps, copied messages, preemption | One shared heap; `asyncio` coroutines and limited threads |
| Faults | A crash stops one actor; a supervisor restarts it | An uncaught exception stops the program unless caught |
| Memory | Per-actor heaps and limits, small collections | One heap and one collector for everything |
| Hardware | Ports (actors that send events) | Python classes and callbacks (`machine.Pin`, IRQ handlers) |
| Updating code | Hot loading of modules with isolation | Copy new `.py` files and restart |
| Size | VM core about 37 KB of code | Larger: it carries the compiler and a big runtime (typically a few hundred KB) |
| Floats, big library, many boards | No floats yet (planned); a small library; two boards | Floats, a large library and ecosystem, many boards |
| Maturity | Young | Over a decade old, widely used |

Choose MicroPython for interactive exploration at a REPL, quick scripts,
floating-point work, its wide board support and its large community.
Choose gogo when a device must keep several activities running and recover
from faults on its own, when you want errors caught before the code reaches
the device, when code must be updated in the field without a restart, or
when the VM has to fit in a small memory budget or inside another program.

### Is gogo compiled or interpreted?
Both, in two steps. `gogo build` compiles `.gg` source ahead of time, on your
computer, into a `.ggb` bytecode image: parsing, type checking and code
generation all happen there, so type errors are found before anything runs.
The VM on the device then interprets that bytecode; there is no native code
generation. This is the model of Java or Erlang/BEAM, not of Go (native) or
Python (source interpreted on the device). It keeps the device small: the
board never needs the compiler, and an image is portable, so the same
`.ggb` runs on the boards, in the host VM and in embedded hosts.

### What is gogovm?
The gogo VM built for your computer (macOS or Linux): the same C runtime
that runs on the boards, compiled as a command-line program: built from
source as `build/gogovm` (`make vm`), or downloaded from a release
(`gogovm-<os>-<arch>`, to put next to `gogo`). It runs `.ggb` images with simulated hardware:
GPIO, keyboard, IMU, IR and the microphone are driven by script lines on
standard input, the network can be simulated or connected, `/flash` and
`/sd` map to host directories (`--fs-root`), and the screen renders in the
terminal. `gogo run` builds a program and runs it in gogovm, which is how
you develop and test without a board; all the golden tests and book examples
run this way. `build/gogovm_mcu` is the same VM with the boards' memory
limits, to check that a program fits.

### Is gogo (or gogovm) an RTOS?
No. gogo is a virtual machine that runs **on top of** an operating system,
not instead of one:

- On the ESP32-S3 boards it runs on ESP-IDF's FreeRTOS: the whole VM is one
  FreeRTOS task pinned to the second core, while Wi-Fi, the LCD renderer and
  the configuration service are ordinary FreeRTOS tasks beside it.
- `gogovm` is a normal macOS or Linux process; an embedded VM runs inside
  whatever thread its host program gives it.

Inside the VM, gogo has its own scheduler for actors. It is preemptive (each
actor runs for a budget of reductions before the next one gets a turn), but
it is not a real-time scheduler: actors have no priorities or deadlines,
interpretation and garbage collection add jitter, and a TLS handshake can
pause other actors for about 200 ms. Work with hard timing requirements
belongs in C, under the RTOS: interrupt handlers, DMA and drivers (audio
I2S, IR timing, the LCD) do the time-critical part and hand results to
actors as messages. In short: the RTOS gives deterministic tasks and
interrupts; gogo adds isolated, supervised, hot-loadable application logic
above them.

### Does gogo have I/O threads?
Not inside the VM. The VM is single-threaded: every actor, every port
driver's commands and every file operation run on one scheduler thread.
I/O that takes time is moved off that thread by the hardware and the
platform, not by VM threads:

- **Interrupts and DMA.** GPIO edges and the keyboard's interrupt line push
  events into a lock-free ring; audio (I2S) and IR (RMT) run on DMA and
  their interrupts hand finished buffers over. The VM only collects the
  results.
- **Platform tasks** on the ESP32-S3, beside the VM's own FreeRTOS task:
  ESP-IDF's Wi-Fi task delivers frames to gogo's network stack, a renderer
  task (`gogo_lcd`) pushes the screen to the LCD over SPI, and a
  configuration task (`gogo_cfg`) serves `gogo config` on USB serial.
- **One poll loop on the host.** `gogovm` waits on its inputs (simulator
  link, stdin script, wake pipe) with one `poll`; an embedded VM is driven
  from whatever thread its host program uses.

Some operations still run synchronously on the scheduler thread and pause
all actors while they last: file reads and writes (a flash write can take
milliseconds), I2C reads of the keyboard and IMU, and the unsliced step of
a TLS handshake (about 200 ms). This keeps the VM small and free of locks;
the cost is that a long blocking operation is felt by every actor. Host
network ports for embedded VMs are planned with an I/O thread that only
reports readiness, so the reads and writes themselves stay on the VM
thread.

### Does the compiler use Go's parser?
Yes. The front end is Go's standard library: `go/parser` for parsing,
`go/ast` and `go/token`, `go/types` for type checking (configured for a
32-bit target, so `int` has the size it has on the device) and
`go/constant`. gogo's own code then rejects what the subset does not
support, resolves imports (`gogo/...` and your own packages) and
requirements, generates bytecode
and writes the image. Syntax and type rules, and the shape of error
messages, are therefore Go's.

### Which Go features are missing, and are they coming?
Expression `switch`, labeled `break`/`continue`, `range` over an int,
variadic functions and packages of your own are supported. Planned, in
this order: methods and field assignment (compiler only); then `panic`, type assertions,
interfaces and `error`; then pointers (boxed, copied when sent), maps (a
library, deterministic order), read-only package variables and `fmt`; then
sized integers, floating point (`float32` and `float64`), arrays, embedded
fields and a static `defer`. Not planned: `goto`, `recover`, per-actor
mutable globals, channels, `go` statements. Generics are deferred.

### Can I import packages?
- **The standard library, yes:** `gogo/actor`, `gogo/http`, `gogo/strconv`,
  `gogo/tui` and the rest are built into the compiler.
- **Your own packages, yes.** An import outside `gogo/` is resolved first
  inside the directory you build (`gogo build app` maps `app/util` to
  `app/util/`), then in each directory of `$GOGOPATH` (colon-separated,
  `;` on Windows; `$HOME/gogo` when unset). The first match wins, with a
  warning when the same path exists in more than one place. No `go.mod`
  is needed. `gogo env` prints the search path and `gogo build -path`
  overrides it (book, section 3.1.1). Fetching packages from the network
  (`gogo get`) and versioning come later.
- **Ordinary Go packages, no.** gogo is a subset of Go, and most Go
  packages use features it does not have (goroutines, reflection, and for
  now maps and floats). Libraries have to be written for gogo.

### Can I write scripts?
Yes, on your computer. `gogo run hello.gg` compiles and runs one file, and
a file starting with `#!/usr/bin/env -S gogo run` runs directly after
`chmod +x` (the compiler ignores a first `#!` line). A script can import
packages next to it and from `$GOGOPATH`. It cannot read command-line
arguments yet. See the book, chapter 2.

### Can I compile on the device?
No. The compiler is written in Go and needs the Go runtime, which does not
fit a microcontroller. You compile on your computer and push the `.ggb`
image to the board (`gogo flash`), or load modules at run time with hot
loading (book chapter 14).

### Can a program that embeds gogo compile source at run time?
Not yet; `libgogo.a` only loads compiled images. Planned: the compiler as a
separate C library (`libgogoc.a`, built with Go's c-archive mode, a few MB
because it carries the Go runtime) with one call that turns source files
into an image plus structured errors, and the Go binding importing the
compiler directly. A host that only runs precompiled images will not need
it.

### Where is the bytecode documented?
The instruction encoding, image layout, type table, versioning and load-time
checks are in [design.md](design.md) §5.6; value representation is in §5.5;
`runtime/vm/opcodes.h` is the authoritative list of opcodes, builtins and
reserved types (the compiler parses it). `gogo dis file.ggb` prints a
program's bytecode. A per-opcode reference is planned.

## The VM

### How does the VM run code?
A register-based bytecode with fixed 32-bit instructions, interpreted by a
`switch` loop. Actors are preempted by counting reductions (1000 per time
slice, counted on backward jumps), so a busy loop cannot starve other
actors. The VM core is about 37 KB of a 64 KB budget.

### Will there be a JIT?
Not planned. On the ESP32-S3 a JIT would need executable internal RAM (which
is scarce and write-protected by default), a code generator for the Xtensa
instruction set, and cooperation with preemption, garbage collection and hot
loading, all within the memory budget; iOS does not allow JIT at all. If
performance becomes a problem, the order is: measure with benchmarks, then
cheaper interpreter dispatch (computed goto, combined instructions), and
moving hot operations into C builtins.

### What happens when the image format changes?
Since format 15, images are versioned `major.minor`. New reserved types and
fields appended to reserved types only raise the minor; a VM runs every
image of its major whose minor is not newer than its own, and the compiler
stamps the lowest minor a program needs, so most new images still run on
older firmware. Hot-loaded modules stay valid across such updates.

## Ports and hardware

### What is a port?
Every peripheral (a pin, the keyboard, the IMU, IR, the microphone and
speaker, sockets, TLS, the screen, ports a host program registers) is a
port: an actor-like endpoint with a PID that you open, send commands to,
and that sends typed events to its owner. Ports close automatically when
their owner exits or crashes. [Book §7.6](book/07-ports-and-peripherals.md)
explains how they work inside.

### Which platforms does gogo run on?
Two different questions: where the **toolchain** runs, and where the **VM**
runs.

| | macOS | Linux | Windows |
|---|---|---|---|
| `gogo` CLI (build, flash, monitor, config, setup) | yes | yes | flash and setup only; no Wi-Fi step or console yet |
| `gogovm` host VM | yes | yes | no (it uses POSIX APIs) |
| `libgogo.a` embedding | yes | yes | no |
| Building firmware from source | yes (ESP-IDF) | yes (ESP-IDF) | ESP-IDF supports it; untested here |

Release binaries of the CLI are built for macOS, Linux and Windows on
amd64 and arm64. Development and testing happen on macOS (Apple silicon);
Linux is expected to work but its full test run is still owed, see
known-issues.

**The VM** runs on:
- **ESP32-S3** under ESP-IDF (v6.1.1) and FreeRTOS: the M5StickS3 and the
  Cardputer ADV.
- **macOS and Linux** as `gogovm`, on 64-bit x86 and ARM.
- **Inside other programs** through `libgogo.a` on POSIX hosts.

Planned: RP2040 (bare metal on the Pico SDK), iOS and Android through the
embedding API and its bindings, and Windows for `gogovm` and embedding
later. The VM itself is portable C99 with no OS dependencies; a platform
supplies a small table of hooks (clock, memory, wake and wait, output) and
its port drivers.

### What hardware does gogo support?
**Boards** (one ESP-IDF firmware tree, `platform/esp32s3`, built per board):

| | M5StickS3 | M5Stack Cardputer ADV |
|---|---|---|
| Chip | ESP32-S3, 8 MB PSRAM | ESP32-S3, no PSRAM |
| Screen | LCD, text console and `screen` port (40×11 cells) | same |
| Input | 2 buttons (GPIO) | 56-key keyboard |
| Battery | gauge through the PM1 power chip | voltage through an ADC divider |
| IMU | BMI270: samples and gestures | same |
| Infrared | send and receive | send |
| Audio | ES8311 codec: microphone and speaker | same |
| Storage | `/flash` (LittleFS) | `/flash` and `/sd` (FAT and exFAT) |
| Network | Wi-Fi, TCP/UDP, TLS | same |

Each peripheral is a port and a named requirement, and each can be left
out of a firmware build with a compile-time switch (`gogo firmware`).

**Hosts:** `gogovm` on macOS and Linux (with simulated GPIO, keys, IMU, IR,
microphone, network, files and screen), and any program that embeds
`libgogo.a` (C99; Rust, Go and mobile bindings planned), which can add its
own peripherals as host ports.

**Planned:** RP2040 boards (the platform layer is being reshaped so a new
board is mostly a small set of hardware hooks), BLE on the ESP32-S3, and
general-purpose `uart`, `i2c`, `spi` and display ports. Other ESP32-S3
boards can be added by describing their pins and peripherals in
`platform/esp32s3/main/board.h`.

### How do I get started on a board?
Install the `gogo` binary and `esptool`, connect the board, run
`gogo setup`. It identifies the board, downloads and verifies the firmware,
flashes it without touching your settings, optionally sets up Wi-Fi, and
runs a first program. See [book chapter 2](book/02-getting-started.md).

### Is there a UI library?
Yes: `gogo/tui`, a small ncurses-like text UI (frames, windows, boxes,
lists, progress bars, a status line, a line editor) on the `screen` port.
The port is written in C (cell grid, LCD flush, terminal and headless
renderers); the library and widgets are written in gogo. See the Text UI
chapter of the book ([chapter 19](book/19-text-ui.md)).

## Embedding and the future

### Can I embed gogo in my own program?
Yes. `make lib` builds `libgogo.a`, a C99 library with one header
(`runtime/include/gogo.h`): create VMs, load images, run or step them from
your own event loop, post messages from any thread, register your own ports,
and give a VM a sandboxed `/flash` directory. Rust, Go and mobile bindings
are planned. See book chapter 18.

### Will actors be able to talk across devices?
Planned. Remote PIDs make `actor.Send` work the same whether the target is
local, on another board, or on a server. The first step is a star: boards
connect to a server node (gogo embedded on the server), which also relays
board-to-board messages, with links and monitors reporting a lost node.
Direct links between boards on the same network, with the server as
directory and fallback relay, are a later direction.

## About the project

### Is gogo vibe-coded AI slop?
Vibe-coded, yes. Slop, no.

Most of gogo's code was written by AI coding agents, directed by its author,
who decides what gets built, makes the design calls and accepts or rejects
the result. "Slop" is code nobody checked; gogo is built so that everything
is checked:

- **Every change is reviewed against a written plan**, by a separate review
  pass that did not write it, and its findings are fixed and re-reviewed
  before anything merges.
- **Tests, not trust.** Golden tests for language features and libraries,
  C unit tests for the VM and drivers, AddressSanitizer, UndefinedBehavior-
  and ThreadSanitizer runs, fuzzing of decoders, and tests that guard the
  image format against unregistered changes.
- **Real hardware.** Features that touch the boards are run on an
  M5StickS3 and a Cardputer ADV before they merge, not only in simulation.
- **Budgets enforced by the build.** The VM core size is recorded on every
  change against a 64 KB limit, and new mutable globals fail the build.
- **Honest docs.** The book's examples are run as tests, the design notes are
  checked against the code, and what does not work yet is written down in
  [known-issues](known-issues.md) instead of being glossed over.

Judge it by its tests and its known issues, not by who typed it.
