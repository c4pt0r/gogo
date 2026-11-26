# gogo FAQ

## Language and toolchain

### Is gogo Go?
No. gogo programs are written in a subset of Go syntax and checked by Go's
own type checker, but they compile to gogo bytecode for gogo's actor VM, not
to native code, and many Go features are not supported (or not yet). That is
why source files end in `.gg` and compiled images in `.ggb`. See
[book chapter 3](book/03-language-basics.md) for the subset and §3.10 for
what is missing.

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
chapter of the book.

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
