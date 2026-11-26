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
signal processing (no floats, an interpreter; do such work in C drivers),
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
would break actor isolation (actors and messages replace them); reflection,
floats and a full runtime would not fit the memory budget. What remains is
the part that maps cleanly onto isolated actors and a small VM. The subset
grows where that holds (methods, interfaces, maps and more are planned; see
above), and where gogo differs from Go it says so.

Dynamic languages (Lua, MicroPython) find type errors only at run time and
are harder to keep small and isolated per task; Rust or C give no isolation
between tasks, no hot loading and a slower edit-test loop on hardware.

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
that runs on the boards, compiled as a command-line program,
`build/gogovm` (`make vm`). It runs `.ggb` images with simulated hardware:
GPIO, keyboard, IMU, IR and the microphone are driven by script lines on
standard input, the network can be simulated or connected, `/flash` and
`/sd` map to host directories (`--fs-root`), and the screen renders in the
terminal. `gogo run` builds a program and runs it in gogovm, which is how
you develop and test without a board; all the golden tests and book examples
run this way. `build/gogovm_mcu` is the same VM with the boards' memory
limits, to check that a program fits.

### Does the compiler use Go's parser?
Yes. The front end is Go's standard library: `go/parser` for parsing,
`go/ast` and `go/token`, `go/types` for type checking (configured for a
32-bit target, so `int` has the size it has on the device) and
`go/constant`. gogo's own code then rejects what the subset does not
support, resolves `gogo/...` imports and requirements, generates bytecode
and writes the image. Syntax and type rules, and the shape of error
messages, are therefore Go's.

### Which Go features are missing, and are they coming?
Expression `switch`, labeled `break`/`continue`, `range` over an int and
variadic functions are supported. Planned, in this order: methods, field
assignment and user packages (compiler only); then `panic`, type assertions,
interfaces and `error`; then pointers (boxed, copied when sent), maps (a
library, deterministic order), read-only package variables and `fmt`; then
sized integers, arrays, embedded fields and a static `defer`. Not planned:
`goto`, `recover`, per-actor mutable globals, channels, `go` statements,
floats. Generics are deferred.

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

### Which boards are supported?
The M5StickS3 and the M5Stack Cardputer ADV (ESP32-S3), plus the host VM on
macOS and Linux and embedding in other programs. RP2040 boards are planned.

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
