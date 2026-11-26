# gogo design

This document describes how gogo works: the language subset, the actor
model, ports, the value representation and memory layout, the image
format, the scheduler and the main subsystems (network, storage, input,
hot loading, audio, TLS). It is the reference for the VM and the
compiler. The user-facing guide is the book ([docs/book](book/README.md));
open limitations are in [known-issues.md](known-issues.md).

## 1. Intent

gogo runs a set of small, isolated actors, written in **Go syntax**, on
microcontrollers (today the ESP32-S3 boards M5StickS3 and Cardputer ADV;
RP2040 boards are planned) to manage sensors, screens, keys and other
peripherals. It borrows AtomVM's core ideas (lightweight processes, a
heap per process, mailboxes, preemption by reductions, links, monitors,
supervision trees, "let it crash") but is **not compatible with BEAM**:
the language, the bytecode and the memory layout are its own.

It is also a research project: the whole system should be small enough
for one person to read completely, and a clean design comes before
features.

## 2. Goals, non-goals, anti-goals

**Goals**
- Actor code is written in Go syntax, in `.gg` files (editors highlight
  them as Go; gopls does not apply). Programs run **only on the gogo VM**;
  running them with `go run` is not a goal.
- Real isolation between actors: an actor that crashes, loops forever or
  exhausts its own memory limit affects no other actor and not the VM.
- Run on small microcontrollers (down to RP2040 class: 264 KB RAM, 2 MB
  flash, Cortex-M0+) and drive real peripherals.
- The same VM runs on macOS and Linux (the host build, for development
  and tests, not a hardware target).
- Readable code size. The VM core (`runtime/vm/*.c`, tests and
  `check32.c` excluded) is about 5.6k lines of C (5 626 in November
  2026) and the compiler about 5.9k lines of Go, tests excluded. Both
  first aimed at about 5k; hot loading, the off-heap `bytes.Bytes`
  table, the port side of the stream protocol and the internal
  invariant checks took them past it, and each was worth its lines. The
  budget that is enforced is the VM core's code size (64 KB on the
  ESP32-S3, `make vmsize`); its line count is recorded beside it, so
  growth stays visible (section 5.10).

**Non-goals**
- Full Go compatibility: channels, generics, reflection, `defer` /
  `recover`, method calls through user-defined interfaces, floating point
  and shared memory between goroutines are out.
- Distribution, multi-core scheduling, JIT.
- Go standard library compatibility; gogo has a small library of its own.

**Anti-goals (failures even if achieved)**
- Becoming a Go compiler clone that keeps adding features for
  compatibility.
- Any way for actors to share mutable state.
- Any single actor's error crashing the whole VM.
- Trading readability for speed with macro tricks (compare AtomVM's
  `opcodesswitch.h`).

## 3. Constraints

- **The VM is plain C99 with no third-party libraries.** It depends only
  on the C library and POSIX; the exception is a board's platform layer,
  which uses the chip vendor's SDK (ESP-IDF) and lives only under
  `platform/<board>`. The VM's test framework is its own (a small
  assertion and test runner). TLS (`runtime/net/tls/`) uses mbedTLS, the
  one external library, and only in the `tls` port.
- **The host tools (the `gogo` command, the compiler) are written in Go
  with only the Go standard library.**
- The same VM sources compile for the microcontrollers and for Unix.
- Memory: **an actor must be as small as possible, no larger than in
  AtomVM.** An idle actor (blocked in `Receive`, no pending messages)
  aims at 160 bytes or less on 32-bit, with a hard limit of 256 bytes; a
  small board should hold 1000 idle actors. For comparison, AtomVM's
  idle process reports 552 bytes in `process_info(memory)` on 64-bit.
- The compiler runs on the computer; the device has no compiler.
- The program image lives in flash and is executed in place: constants
  are not copied into RAM.

## 4. Approach

Three approaches were considered:

| Approach | Description | Trade-off |
|---|---|---|
| **A. Go subset -> own bytecode -> C VM (chosen)** | the compiler is written in Go and uses `go/parser` and `go/types` as its front end | Go syntax, real isolation, fits a microcontroller, readable end to end; costs: only a subset, and an own standard library |
| B. TinyGo, goroutines as actors | only a supervision library to write | no isolation, cooperative scheduling |
| C. TinyGo -> wasm, one WAMR instance per actor | full Go, real isolation | tens of KB per instance; the VM is not our own |

BEAM compatibility (a trimmed AtomVM) and an Erlang-like language of our
own were also considered and rejected in favour of Go syntax.

## 5. Design

### 5.1 Components

1. **The compiler** (`compiler/`, driven by the `gogo` command in
   `cmd/gogo/`; Go standard library only): `go/parser` -> `go/types`
   (32-bit sizes) -> subset check (`compiler/check.go`) -> code
   generation -> a bytecode image (`.ggb`). Sources are the `.gg` files
   of a directory (`compiler.SourceExt`), or one file (`gogo run x.gg`);
   a first line starting with `#!` reads as an empty line, for
   executable scripts. The loader (`compiler/load.go`) parses package
   main from the build root, then loads each import depth first: the
   stub packages and the `gogo/X` libraries from the embedded `lib/`,
   and user packages, every other import path, from disk. A user package
   is found in the build root when the path's first element is the root
   directory's name (`app/util` is `<root>/util/`), else in each
   directory of the search path in turn (`compiler.Config.Path`:
   `-path`, `$GOGOPATH`, or `$HOME/gogo`); the first directory with `.gg`
   files wins, and more than one is a warning (`Program.Warnings`). User
   packages compile exactly like libraries (they join `Program.Libs`, in
   dependency order), so their types are the image's own module types,
   their constant opens count for the requirements and their
   `//gogo:optional` covers their own opens. Their package clause must
   be the directory's name and `main` is only the root's; cycles and
   missing packages are errors naming the chain or every place
   searched. A program that imports no user package compiles exactly as
   before.
2. **The VM** (`runtime/vm/`, C): image loader, interpreter, scheduler,
   per-actor heaps and GC, mailboxes, timers, the port (peripheral driver)
   layer and the module table.
3. **Stub packages** (`gogo/actor`, `gogo/port`, `gogo/bytes`,
   `gogo/net`, `gogo/fs`, `gogo/code`, `gogo/sys`, ...): gogo sources that
   hold only signatures and types (bodies are `panic("gogo only")`), for
   `go/types`. The compiler turns every call into a VM
   instruction or builtin, and their struct types have fixed reserved
   type ids.
4. **Libraries**, written in the gogo subset and compiled into every image
   that imports them: `gogo/supervisor`, `gogo/strings`, `gogo/strconv`,
   `gogo/dhcp`, `gogo/dns`, `gogo/http`, `gogo/sntp`, `gogo/kv`,
   `gogo/ringlog`, `gogo/imulog`, `gogo/wav`, `gogo/sound`,
   `gogo/irproto`. All import paths are under `gogo/`, never a Go
   standard library path. `gogo/strings` and `gogo/strconv` work on bytes
   (no rune decoding). Importing a library compiles all of it into the
   image (no dead-function elimination). Libraries may import stubs and
   other libraries, without cycles. The package table, with each
   package's kind and requirements, is [lib/README.md](../lib/README.md).
5. **Platform layers** (`platform/`): `unix` (the host VM `gogovm`, with
   simulated peripherals) and `esp32s3` (one ESP-IDF firmware for both
   boards). RP2040 is planned.
6. **The embed layer** (`runtime/embed/`, public header
   `runtime/include/gogo.h`): the `gogo_*` API that host programs link
   as `libgogo.a` to run VMs inside their own process (§5.11).

**Repository layout.** `runtime/` is the C runtime shared by all boards:
`vm/` (interpreter and actors), `net/` (the network stack; `net/tls/` is
the TLS client port on top of it), `vfs/`, `stream/`, `input/`,
`console/` (the screen console and fonts), `embed/` (the embedding API's
implementation) and `include/` (`gogo.h`, the one public header). `platform/` holds only
board-specific code (`unix/`, `esp32s3/`). `compiler/` and `cmd/gogo/`
are the Go toolchain (Go module `gogotools`, the repository root). `lib/`
is the library source, `.gg` files, one directory per package (`gogo/X`
is `lib/X/`); its one Go file, `lib/embed.go` (package `gogotools/lib`),
embeds `lib/*/*.gg` into the compiler (`//go:embed`). The Go toolchain
does not read `.gg` files; `tests/lib_test.go` vets the library and runs
its host tests on a copy of `lib/` as a temporary Go module `gogo`.

### 5.2 The Go subset

**Supported today:**
- Types: `bool`; `int` (always 32 bits: the compiler type-checks with
  `types.StdSizes{WordSize: 4, MaxAlign: 4}`, so constant overflow is a
  compile error, and run-time overflow wraps in two's complement as in
  Go); `int64`; `byte` (`uint8`); `string` (immutable bytes); `rune`
  only as the type of character literals (`range` over a string gives
  its bytes, 0..255, with no UTF-8 decoding: the loop variable keeps Go's
  static type `rune`, but its values are bytes); slices; structs; `any`;
  func values and func literals (closures, any signature); named types
  of `int`, `int64`, `bool` and `string` (`type Code int`); `bytes.Bytes` and `bytes.Buffer`.
- Operators: arithmetic and comparison; string concatenation and
  comparison; bitwise `&`, `|`, `^`, `&^`, `<<`, `>>`, unary `^` and the
  compound assignments `&=`, `|=`, `^=`, `&^=`, `<<=`, `>>=`, on `int`,
  `int64` and `byte`. Shifts follow Go: the count may be any integer
  type, a count at or above the width gives 0 for `<<` and 0 or -1 for
  `>>`, a negative count ends the actor with `badshift`; `>>` is
  arithmetic (there are no unsigned types except `byte`), and `byte`
  results are truncated after each operation.
- Slices: `make`, `append` (also `append(a, b...)`), `copy`, `len`,
  `cap`, indexing, element assignment and slicing; `len`, indexing and
  slicing of strings and `Bytes`.
- Statements: `if`, `for` (three-clause, condition-only, infinite, and
  `range` over a slice, a string or an integer), the expression `switch`
  (tagged or not, several values per case, `fallthrough`) on integer,
  `bool`, `string`, PID and Ref values, type `switch` over an `any`
  value, `break` and `continue` (also labeled, on `for` and `switch`; no
  `goto`), `return` with any number of results (named or not), `const`
  and `iota`. The compiler lowers a switch to `EQ`/`EQ64` and `JMPT`
  chains, and labels to jumps: no opcode of their own.
- Functions with several results; variadic functions (the caller packs
  the `...T` arguments into a new slice, the nil slice when there are
  none, or passes the slice of `f(s...)` as it is); func values called
  through `CALLV`.
- Methods with value receivers on named struct types and named basic
  types (`type Celsius int`), in any package of the image; calls
  `x.M(args)`, method values `f := x.M` and method expressions `T.M`.
  Dispatch is static: a method is a function whose parameter 0 is the
  receiver, named `Type.Method` in the image (`app/geom.Point.Add` for a
  user package, `strings.T.M` for a library), so `gogo dis`, crash reports
  and `actor.Reason.Func` show it. `x.M(args)` passes `x` in the register
  right below the arguments; `x.M` is a closure that captured a copy of
  `x` (`MKCLOSURE`); `T.M` is the function itself. Structs are immutable
  values, so a method works on its own copy of the receiver, as in Go. No
  opcode of their own. Pointer receivers and interface types are rejected
  ("pointer receivers are not supported yet", "interface types are not
  supported yet").
- Field assignment: `p.X = v`, `s[i].X = v`, `a.b.c = v`,
  `m.Items[i].X = v`, `op=` and `++`/`--` on a field, and fields in
  multi-assignments (index operands and right-hand sides first, then the
  assignments left to right, as in Go). Structs stay immutable in the
  VM: a write builds a new struct (`GETFIELD` of every other field,
  `NEWSTRUCT`) and stores it back to the variable, or to the element
  with `SETINDEX`; on a path every struct is rebuilt, innermost first.
  A copy made before the write keeps the old value, as Go's value
  semantics require. Each write allocates one struct (2 + fields words)
  per struct on its path, the same as rebuilding it with a literal by
  hand. A field write to a variable a closure captures is rejected
  ("captured variable must not be changed by field assignment"). No
  opcode of its own.
- `println` of any value of the subset except `Bytes` and `Buffer`;
  slices print their elements, `[1 2 3]` (Go's `println` prints
  `[len/cap]address`). Nesting deeper than 16 prints as `{...}` /
  `[...]`, and a value prints at most 65536 terms, then ` ...`.
- Programs: one `package main` (the build root), possibly in several
  files, plus the `gogo/...` libraries and user packages: any other
  import path, found in the build root (`app/util` is `<root>/util/`)
  and then in the search path (`$GOGOPATH`, default `$HOME/gogo`;
  `gogo build -path`), first match wins. No `go.mod`, no versions; a
  package's name is its directory's name (§5.1, the loader).

**Not supported yet (planned for the "language completeness"
milestone):** maps, pointers (and pointer receivers), arrays, integer types other than
`int`, `int64` and `byte` (`uint32` included), package-level variables,
a `switch` on an `any` or a struct value, type assertions, `panic`, the
`error` interface and interfaces other than `any`.
Each is rejected at compile time with a `<what> not supported` error
(`compiler/check.go`). There is no `gogo/fmt` or `gogo/errors` package.

**Out of the design:** channels and `select`, the `go` statement (use
`actor.Spawn`), generics, `defer` / `recover`, reflection, `unsafe`,
floating point.

**Closures capture by value.** A func literal copies each captured
variable into the closure when it is created (the capture slots of its
`K_CLOSURE`), unlike Go's capture by reference. So that both give the
same result, the compiler requires a captured variable to be assigned
only once: any write after its declaration, inside or outside the
closure (`=`, `op=`, `++`/`--`, a `:=` redeclaration), is rejected with
"captured variable must not be reassigned", and so is a write to one of
its fields (`p.X = v`). A write to a field of a captured slice's element
(`s[i].X = v`) changes the shared array, not the variable, and is
allowed. A `return` with values also
writes every named result, so a function whose named result is captured
rejects such returns ("captured named result must not be set by
return"); a bare `return` writes nothing and is allowed. A consequence:
the variable of a three-clause `for` loop cannot be captured (copy it
first, `n := i`).

### 5.3 The actor API (`gogo/actor`)

The authoritative definition is `lib/actor/actor.gg`.

```go
type PID struct{ /* opaque */ }
type Ref struct{ /* opaque; == and != compare */ }
type Reason struct{ Code int; Func string }   // exit reason, §5.8
type Options struct{ MaxMemory int }          // bytes; 0 = the platform default
type ProcInfo struct{ Memory int }

func Spawn(f func()) PID
func SpawnLink(f func()) PID                    // spawn and link atomically
func SpawnOpt(f func(), opts Options) PID       // with a memory limit
func Self() PID
func Send(to PID, msg any)
func Receive() any                              // the next message, FIFO
func ReceiveTimeout(ms int) (msg any, ok bool)  // ms == 0 only looks
func Call(to PID, req any, timeoutMs int) (reply any, ok bool) // request/reply with a Ref
func Reply(req Request, reply any)              // the server side of Call
func Link(p PID); func Unlink(p PID)
func Monitor(p PID) Ref; func Demonitor(r Ref)
func TrapExit(on bool)
func Exit(code int)                             // exit with a reason code
func Kill(p PID, code int)                      // send an exit signal
func MakeRef() Ref
func Register(name string, p PID) bool          // false if taken or p has exited
func Whereis(name string) (PID, bool)
func Unregister(name string)                    // only the registering actor may
func SendAfter(ms int, to PID, msg any) Ref; func CancelTimer(r Ref) bool
func Sleep(ms int); func Now() int64            // ms since the VM started
func Info(p PID) ProcInfo

// System messages
type ExitMsg struct{ From PID; Reason Reason }
type DownMsg struct{ Ref Ref; From PID; Reason Reason }
type Request struct{ From PID; Ref Ref; Body any }
```

Reason codes: `Normal`, `Killed`, `OutOfMemory`, `MailboxOverflow`,
`NoProc`, `DivZero`, `BadCode`, `Shutdown`, `BadShift`, `BadMsg` (a value
that may not cross modules was sent, §5.4.4); user codes start at 100.

A type switch takes the place of Erlang's pattern matching on receive:

```go
for {
    switch m := actor.Receive().(type) {
    case ButtonPressed:
        count++
        actor.Send(screen, ShowText{strconv.Itoa(count)})
    case actor.Request:
        actor.Reply(m, count)
    }
}
```

**Selective receive** exists in one form only: `Call` waits for the
reply carrying its Ref and leaves other messages in the mailbox. There
is no general selective receive (no O(n) scans, simpler semantics).
`Call` does not return early when the server dies; it waits for its
timeout.

**Send semantics.** `Send` deep-copies the message (slices and nested
structs included); the receiver gets an independent copy. This differs
from Go's aliasing and is the price of isolation.
- The copy **preserves internal sharing and cycles** (two fields that
  point to one object point to one copy). It reuses the GC's copying
  algorithm: forwarding marks are written into the source objects during
  the copy and restored afterwards, so no visited table is needed.
- PIDs, Refs, ports, frozen off-heap `Bytes` and constants in flash are
  passed as they are, not copied.
- Closures may be sent (the function and its captures are copied).

**Spawn** deep-copies the closure's captured variables into the new
actor's heap.

### 5.4 Ports (peripherals)

A peripheral is a **port**: a special actor implemented by a C driver.
It has a PID, takes command messages and sends event messages to its
owner. The API is `lib/port/port.gg`: `Open`, `Close`, `Handoff`, the
stream protocol (`Chunk`, `Out`, `Done`, `Closed`) and the host-port
types (`Host`, `Post`). Each device's types are in its own package
(`gogo/gpio`, `gogo/kbd`, `gogo/power`, `gogo/imu`, `gogo/ir`,
`gogo/audio`, `gogo/screen`, `gogo/net`).

```go
led, ok := port.Open("gpio", gpio.Config{Pin: 25, Mode: gpio.Output})
actor.Send(led, gpio.Write{Value: 1})
btn, ok := port.Open("gpio", gpio.Config{Pin: 15, Mode: gpio.InputPullUp, Edge: gpio.Falling})
// the owner then receives gpio.Edge{Pin, Value} messages
```

- `port.Open(driver string, cfg any) (actor.PID, bool)`: the caller
  becomes the owner. An unknown driver, an invalid configuration, a busy
  resource or a lack of memory gives `ok == false` and the zero PID. The
  VM keeps the reason as a shared error code, `vm->open_err`
  (`NotFound`, `BadArg`, `TooMany`, `NoMemory` or `Busy`, `port.h`):
  `fs.Open` reports it, and Go code will get it when `port.Open` returns
  `error` (language phase B).
  `port.Close(p)` closes a port; stale or unknown PIDs are ignored.
  `port.Handoff(p, to) bool`: the owner hands the port to another live
  actor; later events go to `to`, the port closes when `to` exits, and
  the port's accounting (driver state, `Bytes` the port holds) moves
  with it. It returns `false` and changes nothing if the caller is not
  the owner, `to` is not a live actor, or `to`'s quota cannot take the
  accounting. Handing a port to oneself only resumes a paused port (a
  freshly accepted TCP connection).
- Any actor holding a port's PID may send it commands; events go only to
  the owner and count against the owner's mailbox quota. A port closes
  when its owner exits, and the VM closes every open port when it ends.
  `Kill` on a port is `Close`. Ports cannot be linked or monitored;
  `SendAfter` and `Call` to a port are silently ignored.
- `gpio.Config{Pin, Mode, Edge, DebounceMs}`: Mode is `Input`,
  `InputPullUp`, `InputPullDown` or `Output`; Edge is `None`, `Rising`,
  `Falling` or `Both`; `DebounceMs <= 0` means 20 ms. Pins the platform
  uses itself (the screen, the PM1, PSRAM, USB, the SD card, the
  keyboard, ...) fail to open.
- A driver is a table of C functions (open, command, close, and optional
  hooks such as `command_term` for structured commands, `handoff`); new
  peripherals need no change to the VM core. A generic driver (`gpio`,
  the input ports) is one runtime implementation; a board lists an
  instance of it over its hardware layer (`PortDriver.hw`, §5.11.1).
  The port table has a fixed
  size (`GOGO_MAX_PORTS`, 16 by default).
- Interrupt and input events go through a lock-free single-producer ring
  (`GOGO_EVQ_CAP`): an ISR only writes the ring, without locks or
  allocation, and the scheduler drains it each round into messages.
  `platform_wait` has latched-wakeup semantics: a producer calls
  `platform_notify()` (ISR-safe), and no wakeup is lost. Larger sources
  (the network, input, TLS) register as event sources (`PortSource`,
  `runtime/vm/port.h`) that the scheduler drains, and it does not sleep
  while a source reports pending work.
- Unix: `gpio` reads simulated events from stdin (lines `pin value` and
  `sleep ms`; golden tests use an `input` file). `gogo run --key c=pin ...`
  is a terminal mode that maps keys to pins; Ctrl-C and SIGTERM restore
  the terminal first; Ctrl-D ends input. Outputs are printed.
- ESP32-S3: `gpio` uses GPIO interrupts (StickS3: KEY1 = G11, KEY2 =
  G12). On a single-edge pin, the driver (`gpio_edge`, `runtime/gpio/`,
  the same on every board) drops an edge in the same direction that
  arrives within `DebounceMs` after an unreported opposite change
  (release bounce).

Not yet: `uart`, `i2c` and `spi`. The board's screen is a console that
mirrors stdout and stderr; the `screen` port (§5.4.9) takes it over as a
text grid while it is open.

### 5.4.1 The network stack

- **Layers.** `runtime/net/` is plain C (Ethernet, ARP, IPv4, ICMP echo,
  UDP, TCP) and depends only on the platform hooks of
  `runtime/net/netif.h`. Above it are the ports `netif`, `udp`, `tcp`,
  `tcp_listen` and `tls`, and above those the libraries `gogo/dhcp`,
  `gogo/dns`, `gogo/http` and `gogo/sntp`, written in the subset. The
  stub package `gogo/net` (`lib/net/`) holds the port types (reserved
  types 9-22 and 41).
- **Ports.** `netif` (`net.IfConfig{}`, one owner for the whole machine;
  command `net.SetAddr{IP, Mask, Gateway, DNS}`). `udp` (`net.UDP{Port}`;
  command `net.SendTo{Addr, Data}`; event `net.Datagram{From, Data}`, the
  payload a heap `K_BYTES` inside the message block; datagrams over the
  receiver's quota are dropped and counted, never killing it).
  `net.Subscribe()` (a builtin) makes the caller receive `net.Up{IP,
  Gateway, DNS}` and `net.Down{}`. `tcp` (`net.Dial{IP, Port,
  TimeoutMs}`; commands `port.Out{Data}`, `net.Shutdown{}`,
  `net.Resume{}`; events `net.Connected{}` and the stream protocol's
  `port.Chunk`, `port.Done` and `port.Closed{From, Reason}`, §5.4.6) and `tcp_listen`
  (`net.Listen{Port}`; event `net.Accepted{Conn}`: the new connection
  belongs to the listener and stays paused until `port.Handoff` or
  `Resume`).
- **Single receive slot.** The platform (the Wi-Fi task on CPU0) hands
  one frame to the stack's only receive slot with `net_rx_offer` (copy or
  drop; the driver buffer is released at once). Each VM has its own stack:
  all of its state is a `NetState` the board supplies (static on the ESP)
  and attaches with `net_install`; `net_rx_offer` and `net_link_event`
  name the stack they feed. A frame arriving while
  the slot is full is dropped; TCP's one-segment window makes that rare.
  The stack allocates no buffers of its own.
- **TCP.** Windows of one segment each way; only in-order segments are
  accepted. A segment is ACKed only after its payload was delivered into
  the owner's mailbox; a full mailbox means no ACK and the peer resends.
  A `Chunk` (Seq counted per connection, `Dropped` always 0) leaves room
  for control events (two `Done`, two `Closed`), so a slow reader cannot
  be choked by them; one that still does not fit is held in the port
  (`Connected` and the `Closed` reason in `StreamOut.user`, `Done`s owed
  in the `StreamOut`) and retried every 50 ms, in order, and no data goes
  out while `Connected` is held. RTO 1 s, doubling to 8 s, 6
  retransmissions; TIME_WAIT 2 s. The `Out`s live in the port's
  `StreamOut` (`runtime/stream`): one in flight and one queued, a third
  answered with `Done{-1}` and the connection carries on; one `Done` per
  `Out` once the peer acknowledged all of it; the `Out`s still in flight
  when the connection ends get `Done{-1}` before its `Closed`. TCP keeps
  one deadline per connection and reports it to the scheduler through
  its event source rather than using VM timers.
- **Orphan close.** Closing the port after `Shutdown`, or the owner
  exiting, leaves an orphan connection that finishes the close (FIN
  exchange, retransmissions) on its own, for at most
  `GOGO_NET_ORPHAN_MS` (20 s); otherwise a close resets. Losing the
  address or the link (a `SetAddr` with a new address included) ends
  every connection with `NoRoute`; so does the exit of the `netif` owner.
- **Memory** (measured on the ESP32-S3): the stack's fixed state is
  3640 B (budget 4096 B, checked at compile time); a TCB is 56 B, a UDP
  control block 12 B, a `Port` 64 B (32-bit); the net code is about
  11.3 KB (xtensa, -Os). The Wi-Fi driver takes about 30 KB of heap and
  31.5 KB of static RAM; lwIP and esp_netif are not linked.
- **`gogo/dhcp`**: `dhcp.Start(dhcp.Config{TimeoutMs})` starts the DHCP
  client actor, which owns `netif` (renewal at T1, rebinding at T2, the
  address cleared at expiry). With the configuration keys `net.ip`,
  `net.mask`, `net.gw` and `net.dns` it sets a static address instead.
- **`gogo/dns`**: `dns.Lookup(name, timeoutMs) (int, bool)`, served by a
  resolver actor registered as `dns`: A records, 2 retries, a cache of 4
  entries by TTL, cleared when the network goes down or the DNS server
  changes.
- **`gogo/http`**: the client `http.Get`, `http.Post` and
  `http.Do(method, url, contentType, body, maxBody, timeoutMs)` take
  `http://` (HTTP/1.0) or `https://` (HTTP/1.1, through the `tls` port)
  URLs, both with `Connection: close`. Each request runs in a temporary
  actor; the caller waits as with `Call`. The response body is limited
  (`MaxBody`, 8 KB by default; `Do` up to `MaxBodyLimit`, 64 KB), cut
  there and marked `Truncated`, and charged to the caller, whose default
  limit on a board is 16 KB. `NewClient` / `ClientDo` / `ClientUpload` /
  `CloseClient` keep one connection alive; `Upload` streams a file as a
  POST body (§5.4.8). The server `http.Serve(port, handler func(Request)
  Response) actor.PID` starts a listener actor linked to the caller (if
  it cannot listen it exits with code 69, `ListenFailed`, `actor.Failed +
  errors.Busy`) and hands each
  connection to a new handler actor: request head at most 2 KB, body at
  most 8 KB by `Content-Length` (no chunked bodies), 400 for a malformed
  request, 413 for a body too large, then a response with
  `Content-Length` and `Connection: close`, `Shutdown` and exit. Request
  actors are spawned with a limit of 2 x body limit + 6 KB (22 KB by
  default): the body exists in a buffer and in its frozen copy, and the
  GC copies the heap. When an allocation fails against the limit, the VM
  retries once after a tight GC (block = live data + the request, no
  growth slack).
- **`sys.Config`**: `sys.Config(key) (string, bool)` (builtin 48) reads
  the device configuration store; `wifi.` keys are invisible to programs.
  On Unix the store is `$GOGO_CONFIG` (default `~/.gogo/config`, lines
  `key=value`), edited with `gogo config set|get|list|rm` (§5.9).
- **Program arguments** (`sys.Args() []string`): the arguments after the
  program, as Go's `os.Args[1:]`: no element for the program's name, an
  empty non-nil slice when there are none, a new slice on every call. It
  costs the VM nothing: no builtin, no image minor, no host hook. The
  compiler lowers the call inline (`sysArgs` in `codegen_actor.go`) to a
  `sys.Config` lookup of the key `"\x00args"` (`compiler.ArgsKey`), whose
  value a host serves as every argument followed by a NUL byte (empty
  arguments survive; no arguments is an unset key), and a split loop at
  the NULs. Where the host's `config_get` does not know the key (the
  boards, an embedder that did not add it) the result is an empty slice;
  an image that uses `sys.Args` runs on any VM of format 15. `gogovm prog.ggb
  a b` serves the key from `argv` after the image
  (`platform_set_args`, `platform_unix.c`); `gogo run prog a b` passes
  everything after the program to it. The key is also visible to
  `sys.Config`, which is harmless (arguments are public to the program).
- **Wall clock and randomness** (`gogo/sys`, builtins 51-53; `gogo/sntp`):
  `sys.Time() int64` is Unix time in ms (UTC), 0 while unknown.
  `sys.SetTime(ms) bool` sets it, after which it advances with the
  monotonic clock; a time earlier than the build-time floor
  (`GOGO_BUILD_UNIX_MS`, set by the Makefile and the firmware's CMake) is
  refused with `false`, and `ms <= 0` forgets the time. `sys.Random()
  int` returns 30 random bits (`getentropy` on Unix, `esp_random` on the
  ESP). The C hooks are `platform_unix_time_ms`,
  `platform_set_unix_time_ms` and `platform_random`
  (`runtime/vm/platform.h`; `runtime/vm/walltime.c` has weak defaults: an
  offset from the monotonic clock, unknown at boot). There is no
  permission model: any code may set the time, and the floor is the only
  protection. `sntp.Sync(server, timeoutMs) bool` does one SNTP exchange
  in a helper actor (up to 3 sends, 60 random bits as a nonce, source and
  fields checked, half the round trip added) and calls `sys.SetTime` on
  success. On Unix the wall clock starts from the host clock or from
  `$GOGO_UNIX_TIME_MS` (0: unknown); with `--sim-clock` it starts unknown.

### 5.4.2 Storage (`gogo/fs`, `gogo/kv`, `gogo/ringlog`)

One file API over `/flash` (on-chip flash, LittleFS) and `/sd` (SD card,
FatFs).

- **Implementation.** `runtime/vfs/` (plain C: path rules, the open-file
  table, the backend interface `VfsBackend`) plus backends: LittleFS
  (`runtime/vfs/lfs/`, over a board's `BlockDev`) and FatFs
  (`runtime/vfs/fat/`, over a `DiskHal`) on the ESP32-S3; a host directory on Unix (`gogo run --fs-root
  <dir>`; without it every path is `NotMounted`). `gogo/fs` is a stub
  package: the compiler turns its calls into `B_FS` (builtin 49, first
  argument the operation number); `File.Close` is a port close.
  `fs.Info` and `fs.Space` are reserved types 23 and 24.
- **API.** `Open(path, mode) (File, Err)` (`Read`, `Write`, `Append`);
  `File.Read(max) (bytes.Bytes, Err)`, `Write(data any) Err`,
  `WriteAt`, `Sync`, `Size`, `Close`; `ReadFile(path, max)`,
  `WriteFile(path, data)` (writes `<name>.tmp`, then replaces, so after a
  power loss `/flash` holds the old or the new content); `Stat`,
  `ReadDir`, `Mkdir` (not recursive), `Remove` (a file or an empty
  directory), `Rename`, `Usage(prefix)`. Every call returns an `fs.Err`
  (`OK` = 0, `NotFound` ... `NotMounted`: the shared codes of
  `gogo/errors`, `GOGO_E_*` in C). Calls are
  synchronous and stop every actor while they run (a flash erase takes
  tens of ms, an SD access occasionally hundreds).
- **A File is a port.** Its PID is an ordinary port PID owned by the
  opener; it closes (and commits) when the owner exits, crashes or is
  killed, and `port.Handoff` passes it on. At most 4 files are open in
  the whole VM (`TooMany`, `GOGO_FS_MAX_OPEN`); each is charged to its
  owner. Operations by a non-owner give `Closed` and do not crash.
- **Path rules.** Absolute paths with `/flash` or `/sd` as the whole
  first segment; names of 1 to 32 bytes from `A-Z a-z 0-9 . _ -`; a whole
  path of at most 96 bytes. Segments ending in `.` (`.` and `..`
  included), empty segments and a trailing `/` are `BadPath`. Renaming a
  directory into its own subtree (`/flash/d` -> `/flash/d/x`) is
  `BadPath`; renaming a directory while any file is open on the same
  mount is `Busy`. Case folding happens only on `/sd` (FAT is
  case-insensitive), so a case-only rename works there; `/flash` is
  case-sensitive. Mkdir, Remove and Rename of a mount root, any operation
  on `/`, and renames across mounts are `BadPath`. Names created on a PC
  that break the rules are listed by `ReadDir` but give `BadPath` on open.
- **Busy rules.** One `Write` or `Append` open per path at a time;
  `Remove`, `Rename` (source or target) and `WriteFile`'s final replace
  of a path with an open file are `Busy`; on `/sd`, opening a file that
  is being read with `Write` (which truncates) is `Busy` too, `Append` is
  allowed. A reader may open a file that is being written; what it sees
  is not specified. An open file's slot keeps its path's length and
  32-bit hash, not the path; two paths are the same when both match.
- **`gogo/kv`**: one file per key under `/flash/kv/`. `Get(key)
  (bytes.Bytes, fs.Err)` (a missing key is `fs.NotFound`; read errors are
  returned as they are), `Put(key, v any) fs.Err` (`string` or `Bytes`,
  replaced atomically, at most 4096 B: a larger value is
  `fs.Err(errors.Overflow)`, another type `fs.Err(errors.BadArg)`), `Delete(key) fs.Err`, `Keys()
  ([]string, fs.Err)`.
- **`gogo/ringlog`**: `Append(dir, maxBytes, line)` and `Read(dir, max)`
  over two files `0.log` and `1.log` that rotate, so the total stays
  within `maxBytes` (plus one line); one writer per directory.
- **Configuration.** `fs.flash.size` (`gogo config set fs.flash.size 3M`;
  bytes with an optional K or M suffix, rounded to 4 KB, at least 64 KB;
  unset = the whole partition): a size larger than the current file
  system grows it in place at boot; a smaller one is not applied (a
  message says so); `gogo device format-flash` reformats once at the set
  size (an RTC-memory flag set before a software restart; NVS is not
  written), as the deprecated key `fs.flash.format=yes` still does (and
  deletes the key). A first mount that fails formats. The partition
  `gogofs` is at `0x310000`, size `0x4F0000`.
- **`/sd` file systems**: FAT12/16/32 and **exFAT** (ESP-IDF's `fatfs`
  component is overridden with `FF_FS_EXFAT 1`, in the Cardputer build
  only). Long names are limited to 64 characters (`FF_MAX_LFN`). The card
  is mounted on first access and remounted after an I/O error; there is
  no card-detect pin.
- **Boards.** The StickS3 has `/flash` only; the Cardputer ADV has
  `/flash` and `/sd` (SPI at 20 MHz, falling back to 400 kHz). The build
  picks the board with `GOGO_BOARD=sticks3|cardputer`. The fixed state
  (the vfs table plus LittleFS and FatFs static state) is outside the VM
  core: 1104 B on the StickS3, 2416 B on the Cardputer (budget 3072 B,
  asserted at compile time); details in
  [platform/esp32s3/README.md](../platform/esp32s3/README.md).

### 5.4.3 Input (the `kbd` and `power` ports)

- **API.** `gogo/kbd`: `port.Open("kbd", kbd.Config{RepeatDelayMs,
  RepeatMs})`, events `kbd.Key{Code, Char, Mods, Down}` (`Code` is the USB
  HID usage, Fn is 0xF0; presses and repeats have `Down` true, releases
  false). `gogo/power`: `power.Config{PeriodMs}`, events
  `power.Battery{MilliVolts, Pct, Charging, External}`, command
  `power.Read{}`; `Charging` and `External` are 0 or 1, -1 when unknown.
  `MilliVolts` is the average of the last 4 good samples; `Pct` is
  linear from 3300 to 4100 mV. There is no power-button event.
- **Platform hooks.** `runtime/input/` is chip-independent C (key codes
  to characters, modifiers, auto-repeat, battery sampling and backoff).
  The platform describes each device with a table
  (`runtime/input/input.h`, §5.11.1): `KbdHal` (the key table,
  `open/close/pending/read`, optional `live`) and `PowerHal` (`read`,
  and the optional `pending`, `enable`, `live` and `event_driven`), and
  lists `INPUT_KBD_DRIVER(&hal)` and `INPUT_POWER_DRIVER(&hal)` among its
  ports. `KbdHal.pending` only reads; the ISR only sets a flag and calls
  `platform_notify()`; every I2C and ADC access happens on the scheduler
  thread (or before the VM starts).
- **Event source.** `input_install()` registers a `PortSource`; each
  round the scheduler calls `pending` then `drain`. Keys go straight to
  the port's owner, not through the event ring. Power is polled every
  `PeriodMs` on the ESP; on Unix it is event-driven (only `battery`
  script lines and `Read` produce events).
- **Unix.** `kbd` and `power` are simulated from the stdin script or the
  terminal (`platform/unix/input_unix.c`).
- **Cardputer ADV.** TCA8418 keyboard on the shared I2C bus (G8/G9,
  address 0x34, 400 kHz, INT on G11); battery through ADC1 on G10 (12 dB,
  curve-fitting calibration, x2); charging and external power cannot be
  read (-1). G8-G11 are not available to the `gpio` port.
- **StickS3.** No keyboard (`kbd` fails to open). The battery comes from
  the M5PM1 (VBAT at 0x22/0x23, external power at 0x04 bit 0, charging =
  PM1 GPIO0 low), on the shared I2C bus G47/G48 that also powers the LCD.

### 5.4.4 Hot loading (`gogo/code`)

A running program can load another image (a `bytes.Bytes` obtained from
the network, a file or a message) as a **module** and start its entry
functions in new actors by name. Code is never replaced in place: an
upgrade is a supervisor restarting a child with the new module. Book
chapter 14 explains it from the user's side.

**API** (stub package `gogo/code`, compiled to builtin 50 `B_CODE`; the C
side is `runtime/vm/module.c`):

```go
type Module struct{ H int; missing string }   // reserved type 30; a plain value, may be sent
type Err int  // OK BadImage Unsupported NoMemory NotFound BadArg TooMany BadVersion (gogo/errors codes)
func Load(b bytes.Bytes) (Module, Err)
func Spawn(m Module, entry string, arg any) (actor.PID, Err)       // also SpawnLink, SpawnOpt
func Hold(m Module) Err
func Release(m Module)
func Missing(m Module) string    // after Load returned Unsupported: the first requirement the firmware lacks
func MemUsed() int               // code bytes of loaded modules (counted in code.mem)
```

An entry is an exported function of the module's `package main` with
signature `func()` or `func(arg any)`, or `func main()` (exported as
`main`); it is looked up by name, case-sensitively. `arg` is copied into
the new actor by the message rules.

**Lifetime.** Every image has the same format; the boot image is module 0
(same load and start path; held by the VM, never unloaded, not counted in
`code.mem`). A module stays loaded while an actor of its own is alive or
an actor holds it: `Load` makes the caller a holder, other actors call
`Hold`, and a hold ends with `Release` or the holder's exit (at most 4
holders per module). When neither remains, the module is unloaded: its
code memory is freed and the slot's generation increases, so a `Spawn`
through an old handle returns `NotFound`, and a constant, closure or
struct value still pointing into it ends the actor that uses it with
`badcode`. The module table has `GOGO_MAX_MODULES` (8) entries; when it
is full, `Load` returns `TooMany`.
`supervisor.SuperviseModule(m, entry, arg, spec)` holds the module and
restarts the child with `SpawnLink` after a crash; sending it
`supervisor.Upgrade{Module: m2}` (with `actor.Call`; the reply is
`int(code.Err)`) holds the new module, stops the old child, restarts it
from the new module and releases the old one.

**Encoding.** The module is written into the values themselves: constant
refs, closures and a module's own struct type ids carry a 3-bit slot and
an 8-bit generation. Images hold them untagged; the VM adds the tag at
run time (`LOADK` and the like) and checks the module table on every
access, so a stale value is `badcode`. The bit layout, the alias bound and
the cost are in §5.5.3. Images carry a requirement list and an export
table after the constant map (§5.6).

**Cross-module rules** (symmetric: the main program is not special).
When sender and receiver belong to different modules, constant strings,
bytes and boxed `int64`s in the message are copied to the heap; only
basic values, `Bytes`, PIDs, Refs, `Module` values and reserved types
(the structs of the `gogo` stub packages) may cross. A message that
contains a module's own struct type or a closure makes the **sender**
exit with `BadMsg`, for its supervisor to handle; `code.Spawn`'s `arg`
follows the same rule but returns `BadArg`. The check runs when the
message is built (`Send`, `Call`, `Reply`, `SendAfter`, a `Spawn`'s
captures); messages within one module are not affected. A target in an
unloaded module, or a message with a stale value, is `badcode`
(`term_live`). In exit notifications across modules `Reason.Func` is
empty. Only the actor that registered a name may unregister it.

**Requirements.** An image lists, by name, what it needs from the board:
port drivers (`kbd`, `tcp`, `tls`, a host port's name) and mount prefixes
(`/sd`). The compiler infers them (`Requires` in `compiler/modinfo.go`):
the driver of every constant `port.Open`, and `/sd` for constant `/sd`
paths given to `gogo/fs`. An import alone requires nothing; a driver name
known only at run time requires nothing and gives a build warning. The
loader checks each name against the VM's driver list (`port_find_driver`,
which includes registered host ports) and its board's mount list
(`VmBoard.mounts`). The first missing name refuses the image: `Load`
returns `Unsupported` and `code.Missing` names it; the boot image is
checked by the same rule (`gogo: image requires kbd, which this VM
lacks`). A board's capabilities are exactly its port and mount lists,
built with what its build has (§5.4.5); there is no separate bit table.

**The `//gogo:optional` directive.** A package (a library or `package
main`) can declare drivers it uses only softly: opened only when a call needs them at run
time, and their absence (`port.Open` returning `ok=false`) reported as
that call's failure. The directive is a comment line in any file of the
package, `//gogo:optional name...` (by convention at the end of the
package doc comment, after an empty `//` line, as gofmt places
directives).
- It applies only to the declaring package's **own** constant
  `port.Open` calls: those drivers add nothing to the image's
  requirements.
  Importers and the other packages still count their own calls (a
  program that calls `port.Open("tls", ...)` itself requires tls unless
  main names `tls` in its own directive).
- Any driver name is accepted, a board's or a host port's. An empty
  list or a misspelt `//gogo:` directive is a compile error.
- A non-constant driver name is a loophole: the compiler cannot see it,
  so it requires nothing and escapes the load-time check; the build warns
  at each such call (`file:line: port.Open with a non-constant driver
  name: ...`), pointing to `//gogo:optional` as the intended way.
- `gogo/http` declares `//gogo:optional tls` (an image that uses http
  does not require tls; on a firmware without tls, https requests fail);
  `gogo/sound` declares `//gogo:optional mic speaker` (without a microphone
  `Record` returns false, without a speaker `Play` does). No library path
  appears in the compiler.
- `gogo firmware` picks features from the requirements, so it never turns soft
  drivers on by itself; `tls` and audio are on by default, and building
  without them makes those calls fail at run time.

**Code memory and `code.mem`.** The code bytes of all loaded modules
(the boot image aside) are limited by the configuration key `code.mem`
(`K`/`M` suffix; 64 KB by default); past it `Load` returns `NoMemory`.
This memory counts against no actor's limit. Where the code goes:
`module_load` first asks `platform_code_alloc` (`runtime/vm/platform.h`;
the weak default returns `NULL`); if it succeeds, the image is copied
there (on the StickS3, always PSRAM). If it returns `NULL`, a whole,
off-heap, 4-byte-aligned `Bytes` runs in place (the module holds it), and
any other `Bytes` (on the heap, a slice, unaligned) is copied into VM
memory. The count is always the image size. The StickS3 has 8 MB of
octal PSRAM; `platform_esp.c` allocates code with
`heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_32BIT)`. PSRAM is used
only through caps allocation (`CONFIG_SPIRAM_USE_CAPS_ALLOC`, not the
global malloc), so the VM arena, Wi-Fi and the LCD buffers stay in
internal RAM. The Cardputer ADV has no PSRAM and returns `NULL`.

Not provided: permissions (modules have the main program's rights),
signatures (planned: see `docs/known-issues.md`), modules stored in
flash, `sys.Has`, in-place code replacement, cross-module function calls
and shared types. Since a module has every right the main program has,
load only images from a trusted source over an authenticated channel.
`examples/hotload` is the host golden example; `examples/hotload_net`
fetches modules over HTTPS (and refuses plain HTTP) and upgrades on a
board.

### 5.4.5 Optional firmware features

Not every program needs everything in the firmware: an offline display
program needs neither Wi-Fi nor the network stack, a board without an SD
slot needs no FatFs, and an English-only interface needs no 160 KB CJK
font.

- **Build switches** (Kconfig, `platform/esp32s3/main/Kconfig.projbuild`,
  all on by default): `GOGO_NET` (Wi-Fi and the network stack), `GOGO_TLS`
  (depends on `GOGO_NET`), `GOGO_SD` (the Cardputer's `/sd`),
  `GOGO_FS_EXFAT` (depends on `GOGO_SD`), `GOGO_FONT_CJK` (off: a 1.5 KB
  ASCII font is built in and CJK characters are drawn as boxes),
  `GOGO_KBD`, `GOGO_BATTERY`, `GOGO_IMU`, `GOGO_IR`, `GOGO_AUDIO` and
  `GOGO_SCREEN`. A
  switched-off feature behaves as if the board lacked the hardware: its
  `port.Open` returns `ok=false`, and `gogo device scan` and the like
  answer "no network in this firmware".
- **What a build provides.** `platform_esp.c` lists in `esp_ports` only
  the drivers the build has (`#if HAS_KBD && CONFIG_GOGO_KBD`, ...) and
  in `esp_mounts` `/flash` and, with `HAS_SD && CONFIG_GOGO_SD`, `/sd`.
  An image or module that requires anything else is refused at boot
  (`gogo: image requires ..., which this VM lacks`) or by `code.Load`
  (`Unsupported`).
- **`gogo firmware -board sticks3|cardputer [-o dir] [-build] [-with
  list] [-without list] files...`** reads the requirement names of images
  and modules, maps them to features (`net` builds `netif`, `udp`, `tcp`
  and `tcp_listen`; `sd` builds `/sd`; `irtx` builds `ir_tx`; and so on;
  `gpio` and `/flash` are always there; any other name is an error),
  writes an `sdkconfig.gogo` overlay and prints (with
  `-build`, runs) the `idf.py` command. A needed feature cannot be
  removed, and a feature the board lacks is an error. It manages its own
  build directory and deletes an existing `sdkconfig` that disagrees with
  the new switches.
- **Sizes** (`gogo.bin`, ESP-IDF 6.1.1, before audio and TLS were
  added): turning off `GOGO_NET` takes the StickS3 from 883728 to 460352 B
  and its static DIRAM down by about 33 KB; `GOGO_FONT_CJK` off saves
  about 161 KB; `GOGO_SD` off saves 42.6 KB on the Cardputer; with every
  optional feature off the StickS3 is 298704 B and the Cardputer 276160 B
  (about -66 % and -70 %). Current figures per feature are in
  [platform/esp32s3/README.md](../platform/esp32s3/README.md).

### 5.4.6 IMU, infrared and streams

- **The stream protocol** (`runtime/stream/`, plain C), spoken by every
  port that moves bytes: tcp, tls, imu, ir, mic, speaker and (with its
  own owed rule) screen. Every event carries `From`, the port's PID, so
  one actor can own several such ports. Input streams deliver
  `port.Chunk{From, Seq, TimeMs, Dropped, Data}` (`Seq` counts dropped
  chunks too; a chunk that does not fit the mailbox is dropped and
  counted, never killing the receiver; tcp and tls build theirs with
  `stream_chunk_new` and refuse the segment instead, so they never drop).
  Output streams take `port.Out{Data}` commands; every accepted `Out` is
  answered with a `port.Done{From, Seq}` (sent ones numbered from 1;
  refused or failed ones -1; a refusal `Done{-1}` may be dropped when the
  receiver reads nothing and already owes `STREAM_OWED_MAX`, 8, as many as the screen port). At most
  2 `Out`s per port are in flight, an `Out` whose `Done` is owed
  included; a third gets `Done{-1}` at once. A `Done` that does not fit
  the mailbox is owed and delivered later, in order: the one owed rule of
  every acknowledging port. The Dones of accepted Outs come in the Outs'
  order; a refusal is answered at once and may come before the Dones of
  Outs still in flight. While the driver holds events of its own
  (`StreamOut.user`: tcp's held `Connected` or `Closed`) every `Done` is
  owed, so none overtakes them. A port that ends by itself sends
  `port.Closed{From, Reason}` (tcp and tls, with the `net` reasons, and
  a host port the embedder ends with `gogo_port_close`, with the reason
  it gives; a failing microphone is meant to, later).
  Reserved types 31-34.
- **`imu` port** (`gogo/imu`): `imu.Config{RateHz, Gestures, BatchMs}`.
  `RateHz` is 0, 25, 50, 100 or 200; a sample is 12 bytes `gx gy gz ax ay
  az` (int16 LE, +-2000 dps and +-8 g, the chip's own axes), collected
  into one `Chunk` per `BatchMs` (default 200, clamped to 50..1000); the
  batch buffer comes from the VM allocator and is charged to the owner (at
  most 2400 B). Gestures arrive as `imu.Gesture{Kind, Value}`; the bits
  are `Tap DoubleTap TripleTap Orientation Flat AnyMotion NoMotion Step`.
  For `Orientation` the low two bits of `Value` are portrait / landscape
  left / portrait upside down / landscape right, +4 for face down; for
  `Step`, `Value` is the step count. The port polls every `min(BatchMs,
  50)` ms.
- **`ir_tx` and `ir_rx` ports** (`gogo/ir`, `gogo/irproto`): timings are
  uint16 LE microseconds, alternating mark and space, starting with a
  mark, each 1..32767, at most 256 values per frame, the same format both
  ways (a received frame can be replayed as is). `ir.TX{CarrierHz,
  DutyPct}` takes `Out`s; `ir.RX{GapUs}` delivers one `Chunk` per frame,
  and a frame that is too long is cut and counted in `Dropped`.
  `gogo/irproto` encodes and decodes NEC.
- **Platform hooks.** `ImuHal` (`open/close/read_fifo/read_gestures`),
  `IrTxHal` (`start/busy` and the optional `open/close`, called when a
  transmit port opens or closes; the ESP uses them to reference-count the
  StickS3's EXT_5V; NULL on Unix) and `IrRxHal` (`open/read/close`).
  Signatures and contracts are in `runtime/input/input.h`.
  Unix simulates them from the stdin script (`imu sample`, `imu
  gesture`, `ir rx`); `ir_tx` prints to stderr.
- **ESP32-S3.** Both boards have a BMI270 on the shared I2C bus (probed at
  0x69, then 0x68; polled only). The first open uploads Bosch's legacy
  configuration (8192 B, BSD-3, license header kept,
  `bmi270_legacy_config.c`), the only one with tap and orientation. IR
  uses the RMT (1 MHz): TX on G46 (StickS3) and G44 (Cardputer); RX only
  on the StickS3 (G42, 3 frame slots, receive re-armed in the completion
  callback). On the StickS3, the PM1's EXT_5V is on while any IR port is
  open (reference-counted, off after the last close; the Grove/Hat 5 V
  pins are outputs then and must not be fed), and the speaker amplifier
  (PM1 GPIO3) is off while `ir_rx` is open.
- **Background recording** (`gogo/imulog`, written in the subset on
  `gogo/fs`): `imulog.Start(cfg, dir, maxBytes)` opens the imu port and
  hands it to a logging actor that appends every `Chunk` and gesture as a
  binary record (a 15-byte header `kind, a, timeMs, b, len` plus the
  payload) to `dir/0.imu` and `dir/1.imu`, rotating like `ringlog`, so
  the total stays within `maxBytes` plus one record; `imulog.Read(dir,
  max)` returns the newest `max` records.
- **Switches.** Kconfig `GOGO_IMU` and `GOGO_IR` (on by default) build
  the `imu`, `ir_tx` and `ir_rx` drivers. With `GOGO_IR` off
  the RMT driver is not linked. The IMU costs about 10.5 KB of flash
  (8 KB of it the configuration) and IR about 25.8 KB (mostly IDF's RMT
  driver).

### 5.4.7 Audio (the `mic` and `speaker` ports)

- **Ports** (`gogo/audio`, reserved types 39-40). PCM is always 16-bit
  little-endian mono; `RateHz` is 8000 or 16000. `mic`: `audio.Mic{RateHz,
  FrameMs, GainDb}`; `FrameMs` 0 means 100, otherwise it is clamped to
  20..500. Each full frame (`RateHz * FrameMs / 1000 * 2` bytes) is sent as
  a `port.Chunk` whose `TimeMs` is the frame's nominal start (the open
  time plus `FrameMs` per earlier frame, dropped ones included).
  `Dropped` counts whole frames: chunks that did not fit the mailbox, and
  bytes lost to a hardware overflow (rounded up to frames). On an
  overflow the partial frame before it and the data after it up to the
  next frame boundary are discarded, so every `Chunk` is contiguous and
  its `TimeMs` exact. `speaker`: `audio.Speaker{RateHz, VolumePct}`; the commands
  are `port.Out`s holding PCM, and each `Done` comes when its `Out` has
  really been played (not merely written to a hardware buffer). With two
  `Out`s in flight the second follows the first without a gap. Closing
  (or the owner's exit) cuts playback at once, without `Done`.
- **Half-duplex.** One `mic` or one `speaker` at a time; a second open is
  refused before any hook is called, and the open one is unaffected. Both
  boards have one ES8311 codec on one I2S bus.
- **GainDb and VolumePct** are relative to the platform, not codec values.
  `GainDb` (clamped to 0..24, rounded down to 6 dB steps) is added to
  the platform's "normal" microphone level, at which speech at arm's
  length is well above the noise floor without clipping. `VolumePct` 0 is
  mute; 1..100 runs from 40 dB below the platform's maximum level to the
  maximum. A different codec later needs no program change.
- **Memory.** The frame buffer is allocated from the VM allocator at open
  and charged to the owner (`Port.charge_words`; F = frame bytes / 4
  words); each `Chunk` waiting in the mailbox costs F + 10 words more.
  Open also checks that the owner has room for one `Chunk` beside the
  buffer (`Port.room_64w`, in `Port`'s padding, so `Port` stays 64 bytes)
  and returns `ok=false` otherwise, rather than staying silent forever. To
  survive a stall of S ms (writing to SD), the owner needs heap + F +
  (ceil(S / FrameMs) + 1) x (F + 10) words: at 16 kHz and 100 ms, a 300 ms
  stall needs heap + 4040 words, more than the default 4096-word limit on
  a board, so `gogo/sound`'s recorder starts with `actor.Options{MaxMemory}`
  and opens `mic` itself.
- **Platform hooks.** `MicHal` (`open/read/close`) and `SpkHal`
  (`open/write/queued/close`) (contracts in `runtime/input/input.h`;
  `close` and `queued` optional; a board without the device lists no
  driver for it), all called on the scheduler thread. `MicHal.read` may
  return a single DMA buffer, fewer bytes than asked, and reports bytes
  lost to a full buffer in `*lost`; `SpkHal.write` does not block, and
  `queued` returns the bytes written
  but not yet played, from which the common code knows when an `Out` has
  finished. The common driver is `runtime/input/audio.c`, and all data
  goes through `runtime/stream/`. Unix simulates the ports from the stdin
  script (`mic tone <hz> <ms>`, `mic silence <ms>`) and prints speaker
  writes to stderr; `--fs-quota` (test only) simulates a full disk (a byte quota per medium).
- **ESP32-S3** (`audio_esp.c`, Kconfig `GOGO_AUDIO`, on by default):
  ES8311 on the shared I2C bus (0x18); I2S_NUM_0 as master, Philips,
  simplex, created at open and deleted at close; the codec is clocked by
  BCLK. `mic` uses 11 DMA buffers of 30 ms (960 B at 16 kHz, internal RAM,
  allocated at open), which survives a 300 ms stall; the first 2 buffers
  are discarded. `speaker` uses 4 DMA buffers of 10 ms plus an 8 KB ring, for
  at most 40 ms of latency. `GOGO_AUDIO` selects
  `CONFIG_I2S_ISR_IRAM_SAFE` (callbacks in IRAM); otherwise during a
  `/flash` write the GDMA reports only the last finished buffer and data
  is lost or replayed. The StickS3's amplifier (AW8737, PM1 GPIO3) is
  turned on last at open and muted then turned off first at close; `speaker`
  cannot open while `ir_rx` is open. The StickS3's maximum level is
  capped at DAC -4 dB (`AUDIO_SPK_MAX_DB`), the Cardputer ADV's is 0 dB.
  The codec's analog reference stays on after the first open, so only
  the first `mic` open after power-up has about 1 s of zeros. With
  `GOGO_AUDIO` off, IDF's I2S driver is not linked and the board lists
  no `mic` or `speaker` driver.
- **`fs.File.WriteAt`** (`B_FS` operation 13) overwrites bytes of a file
  opened with `Write`, without extending it or moving the append
  position; the host (`pwrite`), FatFs, LittleFS and memfs backends behave
  the same. It needs no new space, so a WAV header can be fixed on a full
  SD card; on `/flash` (copy-on-write) it needs one free block.
- **`gogo/wav`** (subset library): `Create(path, rate, planned)` writes a
  standard 44-byte header for the planned length, `Write` appends PCM,
  and `Close` fixes the header in place with `WriteAt` when the real
  length differs; `Info` parses a header.
- **`gogo/sound`** (subset library): `sound.Record(dir, cfg, maxSec)`
  starts a recorder actor (it opens `mic` itself; `Record` waits for its
  answer with `actor.Call`, so the open's charge falls on the recorder's
  `MaxMemory`) that writes each `Chunk` to `dir/NNNN.wav`, starts a new
  file every `maxSec` seconds (default 600, at most 3600) and sends a
  `sound.Recorded` per file; `sound.Stop{}` ends it, followed by exactly
  one last `Recorded`. On a write error or a full disk it closes the file
  properly, reports the error in `Err` and ends, deleting nothing.
  `sound.Play(path)` opens `speaker`, keeps two 4096-byte `Out`s in flight
  and answers with a `sound.Played` at the end. Both actors close their
  port before their last message, so the caller can open the other
  direction at once. `gogo/sound` declares `//gogo:optional mic speaker`.

### 5.4.8 TLS (the `tls` port) and HTTPS

The contract is at the top of `runtime/net/tls/tls.h`; book chapter 17
explains it from the user's side.

- **Port.** `tls`, configured with `net.DialTLS{IP, Port, TimeoutMs,
  Host}` (reserved type 41). Its messages are exactly those of `tcp`
  (`Connected`, `Shutdown` and the stream protocol's `Out`, `Done`,
  `Chunk`, `Closed`), with TLS plaintext as the bytes. It is a mode of the `tcp` driver (a
  `TcpUpper`), not a second stack: TCP's events, retries, write rules and
  deadlines all apply, and the owner sees `tcp`'s semantics. TLS 1.2
  client only (mbedTLS 4.x's TLS 1.3 key exchange cannot be sliced), with
  ECDHE and AEAD suites.
- **Sliced handshake.** Each drain of the network stack runs one
  `mbedtls_ssl_handshake_step`. The ECDSA verifications
  (ServerKeyExchange and the certificate chain) are split with PSA's
  interruptible operations (`GOGO_TLS_MAX_OPS` per slice), and a slice
  that gives up continues at the next drain. While work remains, the
  port source's `pending()` stays true, and `vm_step` checks it before
  reporting idle, so the next slice runs at once. **Not sliced:** the ECDHE key
  generation plus agreement (one step) and each RSA signature check.
- **Verification is always on.** The platform provides the trust anchors
  (`platform_tls_verify_setup`); validity dates are checked against the
  wall clock (`gogo/sys`, set by `gogo/sntp`, with the build time as a
  floor) and the name against `Host` (also sent as SNI). Without a clock
  the result is `Closed{NoTime}`, a bad certificate is `Closed{BadCert}`,
  any other TLS failure `Closed{TLS}`. There is no "don't verify" mode.
- **Memory.** Each connection charges its owner about 23 KB (16 KB in and
  4 KB out of record buffers plus the connection state); the measured
  peak is about 34-35 KB, the difference being handshake temporaries,
  freed when the handshake ends and not charged. One TLS connection
  exists at a time per VM (`GOGO_TLS_MAX` 1; the connection lives in the
  VM's net stack state).
- **One `TlsHw` per process.** mbedTLS's allocator, time and RNG hooks
  are process-global and take no context, so they are bound to the first
  board TLS table (`TlsHw`) that sets mbedTLS up. Every VM in one process
  must use that same table: a VM whose board brings another one has its
  `tls` opens refused (`ok=false`, logged once). This is the runtime's
  one piece of process-global state. VMs on different threads may still
  run TLS at the same time: on the host the time hook reads a
  thread-local pointer to the VM being driven, so each VM checks
  certificates against its own wall clock, and the host's mbedTLS is
  built thread-safe.
- **`gogo/http`.** `https://` uses the `tls` port. One-shot `Get`, `Post`
  and `Do` send HTTP/1.1 with `Connection: close` over https (HTTP/1.0
  over http), because some servers (Cloudflare among them) answer an
  HTTP/1.0 request with a body delimited only by the connection close,
  which is refused over TLS (a close can be forged at a record boundary).
  `NewClient` / `ClientDo` keep the connection and handshake once;
  `Upload` / `ClientUpload` stream a file. Actors that open a tls
  connection are spawned with `http.TLSMemory` (26 KB) more memory.
- **ESP32-S3** (Kconfig `GOGO_TLS`, on by default, depends on
  `GOGO_NET`). The firmware uses ESP-IDF's own mbedTLS component (4.1.1,
  the same source as the host build) rather than a second copy: hardware
  AES, SHA and MPI and the certificate bundle come with it, and Wi-Fi's
  WPA links it anyway. The configuration is in `sdkconfig.defaults`
  (restartable ECC, TLS 1.2 client only, record buffers 16384 / 4096
  without IDF's dynamic buffers, which would move the output buffer the
  port sends from, X25519, `HAVE_TIME_DATE` so that validity is checked
  at all, mbedTLS built with -O2). A build without TLS adds
  `sdkconfig.tls-off`, which restores IDF's defaults for the settings that
  also affect Wi-Fi's WPA crypto (-O2, the allocator, restartable ECC,
  time); `gogo firmware` adds it by itself. `tls.c` is built with
  `GOGO_TLS_MBEDTLS_HOOKS=0` and installs no hooks into the shared mbedTLS;
  the platform sets it up:
  - **Memory:** `CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC`; `platform_esp_init`
    first installs the `platform_tls_alloc` allocator, which prefers PSRAM
    on the StickS3 (a handshake leaves internal RAM untouched; about
    35 KB of PSRAM at the peak) and uses internal RAM on the Cardputer.
    `gogo device stats` shows `mbedtls_mem`, `mbedtls_mem_peak`,
    `psram_*` and `tls_*`.
  - **Time:** the wall clock is the system clock (`settimeofday` /
    `gettimeofday`), which mbedTLS reads with `time()`, consistent with
    `gogo/sys`; `platform_unix_time_ms` returns 0 until it is set.
  - **Randomness:** IDF's mbedTLS draws keys from `esp_fill_random` (a
    CSPRNG while Wi-Fi is on); `platform_tls_random` uses the same source
    and only guards open.
  - **Trust anchors:** IDF's certificate bundle in flash. The chain is
    checked link by link (restartably) up to the last certificate the
    peer sent; if that one is itself a bundle root (same subject and key;
    the shared rule in `runtime/net/tls/anchor.c`, host-tested through
    `$GOGO_TLS_BUNDLE_FILE`), it is trusted (a root cross-signed by an
    older root, as for example.com), otherwise its signature is checked
    with its issuer's key from the bundle (one step). IDF's
    "cross-signed" mode is off: it verifies through a CA callback, with
    which mbedTLS checks the whole chain in one unsliced call.
  - **lwIP:** x509 calls `inet_pton` to match IP-address names, which IDF
    maps to lwIP and would link all of lwIP (whose `tcp_*` names clash
    with gogo's stack); the platform defines `lwip_inet_pton` (dotted
    IPv4 only).
- **Size.** TLS on adds about 140 KB to `gogo.bin` (the bundle 66 KB,
  mbedTLS -O2 about 28 KB); see "TLS" in
  [platform/esp32s3/README.md](../platform/esp32s3/README.md). With it
  off, neither `tls.c` nor mbedTLS's TLS layer is linked.
- **Measured** on the StickS3 against example.com
  (`examples/httpsdemo`): a handshake takes about 2.5 s, nearly all of it
  software ECC (the chain about 1.75 s, ServerKeyExchange about 0.35 s);
  sliced steps take 11-26 ms, a few that cannot be split further about
  50 ms; the unsliced ECDHE step (X25519) takes 204 ms (about 300 ms with
  P-256). A keep-alive `http.Client` handshakes once and later requests
  take about 0.3 s.

### 5.4.9 The text screen (the `screen` port, `gogo/tui`)

Book chapter 19 is the user's side; `runtime/screen/screen.h` and
`screen_port.h` hold the contracts.

- **Model.** A grid of cells, like ncurses, not pixels: each cell a BMP
  code point, a 16-colour foreground and background and the attributes
  bold, underline and reverse, in one `uint32_t` (1760 bytes for the
  boards' 40 x 11 of 6 x 12 pixel cells). A wide character takes two cells
  (the console's width rule; box drawing and block elements are narrow);
  overwriting either half blanks the other.
- **Port.** `screen`, configured with `screen.Config{Cols, Rows}`
  (reserved type 44, format 15.1; 0 = the whole display). One per Vm, refused when the
  board's hook says the display is busy. The grid is allocated at open and
  charged to the owner. The owner first gets a `port.Chunk` (cols, rows,
  colours, attributes). Every `port.Out` is a frame of byte-coded
  operations (clear, move, text, colour, attributes, fill, clip, scroll,
  cursor; `lib/screen/screen.gg`): checked whole, then applied at once, the
  data read in place on the sender's heap (no copy, no held Bytes), and
  answered with `port.Done` (-1: malformed, nothing applied). A Done that
  does not fit the mailbox is owed and retried every 10 ms by the port's
  event source (GOGO_MAX_SOURCES is 3 for it: the boards already use two),
  after collecting a waiting owner's heap slack as stream/ does; with 8
  owed, further Outs get Done{-1}, counted, so every Out gets its Done. Every operation is
  clipped to the clip rectangle, which is always inside the grid, so no
  sequence, malformed or not, writes outside the cell array (fuzzed in
  `make test-screen`). Only the rows whose cells differ at the end of a
  frame from its start are handed to the display (compared by a 64-bit
  row hash, so a clear-and-redraw costs nothing on the LCD).
- **Display hooks.** A board passes a `ScreenHw` (open, present the dirty
  rows, close) through its own open function (`SCREEN_PORT_DRIVER`); the
  VM core only gains the reserved type, the capability name and the
  third event-source slot. ESP32-S3 (`console_task.c`): `present` copies the
  changed rows into a grid shared with the console's render task, under
  the console's spinlock, one row per hold; the render task draws them
  with `screen_render_row` (RGB565, box drawing and blocks drawn as lines
  and fills, bold as a one-pixel double strike) instead of the console,
  and redraws the console whole on close. Unix (`screen_unix.c`): ANSI on
  a terminal (alternate screen, SGR colours), else a text dump of every
  frame for goldens (`--screen ansi|text|none`).
- **Library.** `gogo/tui` is gogo code: frames that share the screen's
  `bytes.Buffer` and track the port's clip, colours and attributes, windows
  (an origin and a clip), widgets as value structs and functions (Label,
  Box, List, progress bar, status line, Edit), and `tui.Key` over the
  keyboard or two buttons. It never receives on its own (`Event` maps a
  message the caller received and takes only as many Dones as the screen
  has Outs unanswered; `Open` reads the size Chunk in a helper
  actor that hands the port over), so a UI composes with other messages.
  It declares `//gogo:optional kbd`.
- **Switch and sizes.** Kconfig `GOGO_SCREEN` (on): the board lists the
  driver, the requirement `screen`; `screen.Config` is image format 15.1. `runtime/screen` is 4.3 KB of code on the
  ESP32-S3 with no fixed state (`make screensize`); the firmware grows by
  about 5.1 KB, plus the 1760-byte shared grid allocated at the first
  open.

### 5.5 Value representation and memory

- **One 32-bit tagged word format** on the host and the microcontroller.
  Small integers live in the word (31 bits, for `int` and `int64`); values
  outside that range are boxed automatically. An `int64` put into `any`
  is always boxed (the compiler picks the `*64` opcodes by static type
  and inserts `BOX64` at the conversion), so a type switch can tell `int`
  from `int64`. Other composite values are references into the actor's
  heap.
  - Tagged words rather than raw values typed statically: the GC needs no
    stack maps from the compiler and the VM stays simple; the cost is
    boxing some integers.
- **Heap references are word offsets, not native pointers**, so the
  memory layout on a 64-bit host is the same as on the microcontroller.
- Structs carry a header with their type id on the heap (for type
  switches).
- **Each actor has one contiguous block** holding its heap and its stack
  frames. When space runs out, a Cheney copying GC runs and grows or
  shrinks the block by policy. Each actor has a memory limit; exceeding
  it ends the actor with `OutOfMemory`.
- **Zero-copy large data goes through one type only.** A
  `gogo/bytes.Buffer` (writable, owned by the current actor) becomes an
  immutable `bytes.Bytes` with `Freeze()`. A `Bytes` larger than 64 bytes
  (`GOGO_BYTES_INLINE`) lives off the heap, reference-counted, and a send
  passes only the reference. Ordinary `[]byte` and `string` values are
  copied when sent (string constants in flash are not).
- **Messages** are deep-copied into a separately allocated message block
  that is linked into the receiver's mailbox; `Receive` merges the block
  into the receiver's heap. The VM is single-threaded and needs no atomic
  operations.
- **Accounting.** Queued messages count against the **receiver's** limit
  (heap + mailbox). A receiver over its limit exits with
  `MailboxOverflow` (its supervisor decides whether to restart it); the
  sender is not affected.
- **Machine out of memory.** When an allocation fails, the actor that is
  allocating is killed (`OutOfMemory`). The VM keeps a small emergency
  reserve (`GOGO_RESERVE_BYTES`, 1 KB) so that exit processing and the
  exit and down messages still complete; the VM itself does not crash.
- Constants in the image (strings, constant tables) are referenced in
  flash, not copied.
- **GC roots and dead registers.** The GC scans every register of every
  live frame, so a dead register that still holds a heap value keeps it
  alive. The compiler therefore emits `CLEAR a n` (`ra..ra+n-1 = nil`) at
  the end of each statement (for freed temporaries, discarded call
  results and, at the end of a block or loop, its locals) and before
  every backward jump, merged into one instruction per contiguous range.
  Jumps carry the "to clear" set to their target label, so paths that
  leave through `break`, `continue` or a conditional jump are cleared too.
  Registers that never hold heap objects (bool, PID, constants) need no
  clearing. Register allocation is lexically scoped: every register
  below the current allocation point is a live local of an enclosing
  scope and is never cleared.

**Values by static type**

| Static type | Zero value | Term | Into `any` | GC | Message copy |
|---|---|---|---|---|---|
| `int` | small int 0 | small-int immediate within 31 bits; a `K_INT` heap box beyond | as is (`TYPEIS` -> `int`) | immediates not scanned; a `K_INT` box is copied, its content not scanned | immediates as is; boxes deep-copied |
| `int64` | small int 0 | immediate within 31 bits; a `K_INT64` box (heap or constant area) beyond | always `BOX64` to a `K_INT64` box (constants boxed at compile time in the constant area), to tell it from `int` | as `int` | as `int`; constant-area boxes as is |
| `bool` | `T_FALSE` | the specials `T_FALSE` / `T_TRUE` | as is | not scanned | as is |
| `string` | `T_EMPTYSTR` | a `K_STRING` heap object or a constant-area ref | as is | heap strings copied (bytes not scanned); constant refs untouched | heap strings deep-copied; constant strings as is |
| `byte` (`uint8`) | small int 0 | small-int immediate 0..255; `byte` arithmetic is truncated after each operation | not allowed (indistinguishable from `int` at run time; rejected at compile time) | not scanned | as is |
| `rune` (`int32`) | small int 0 | small-int immediate; only the type of character literals (the `range`-over-a-string variable is typed `rune` but holds bytes, 0..255) | not allowed | not scanned | as is |
| `[]T` (slice) | `T_NILSLICE` = `0x52` (`s == nil` is true) | a `K_SLICE` heap object `[hdr][off][len][cap][array]` with raw off/len/cap; `array` is a `K_ARRAY` `[hdr][n][elems]` (elements scanned as terms) or a `K_BYTEARR` `[hdr][nbytes][bytes]` (`[]byte`, bytes not scanned), or `T_NIL` at capacity 0; an empty non-nil slice is a `K_SLICE` with cap 0 | as is | the header and the backing array are copied; `K_BYTEARR` bytes not scanned | deep copy preserving sharing and cycles (aliases hold within a message, never across actors) |
| `bytes.Bytes` | `T_EMPTYBYTES` = `0x62` | a heap `K_BYTES` `[hdr][nbytes][bytes]` (immutable, not scanned; `Freeze` results up to 64 bytes and the results of `FromString` / `FromSlice`), or, for a larger `Freeze` result, an off-heap handle with tag `1110`: `(index << 4) \| 0xE` into the VM-wide reference-counted table | as is (`case bytes.Bytes` matches `TY_BYTES`) | heap bytes not scanned; handles' holders recomputed at GC, released when unreachable | heap ones deep-copied; handles passed by reference (count + 1), the receiver charged their full size (queued ones in the mailbox) |
| `bytes.Buffer` | not usable (methods on the zero value are `badcode`; use `NewBuffer`) | a `K_BUF` `[hdr][len][array]` with raw `len`; `array` is a `K_BYTEARR` (its capacity), `T_NIL` at capacity 0; `Freeze` copies once and empties it | as is | array bytes not scanned | deep copy |
| struct (the reserved `actor.*`, `port.*`, ... types included) | a `K_STRUCT` with each field's zero value | `K_STRUCT` heap object `[hdr][type id][fields...]`; a module's own type ids carry the module tag, reserved types do not (§5.5.3) | as is (`TYPEIS` compares the type id) | fields scanned | deep copy preserving sharing and cycles |
| `any` | `T_NIL` | the term of the dynamic value | - | by the dynamic value | by the dynamic value |
| func value (any signature) | `T_NIL` (`Spawn` and calls are `badcode`) | a `K_CLOSURE` heap object `[hdr][module tag \| function index][captures...]`; `CALLV` passes the captures as leading arguments, then the arguments | not allowed (rejected at compile time) | captures scanned | deep copy |
| `actor.PID` | `T_ZEROPID` = `0x6` (slot 0, generation 0: slot 0 never has generation 0 and port slots are >= `0xFFF0`, so it equals no live actor or port) | PID immediate `(gen << 20) \| (slot << 4) \| 0x6` | as is (`TYPEIS` -> `PID`) | not scanned | as is |
| `actor.Ref` | `T_ZEROREF` = `0x42`, a special whose serial counts as 0 | a `K_REF` heap object `[hdr][serial lo][hi]`; serials start at 1 | as is (`TYPEIS` -> `Ref`, the zero value too) | a `K_REF` is copied, content not scanned; `T_ZEROREF` not scanned | `K_REF` deep-copied (same serial); `T_ZEROREF` as is |

Zero PID and Ref: `Send` to the zero PID is dropped; `Link` or `Monitor`
of the zero PID behaves as for a dead PID (`NoProc`); `CancelTimer` and
`Demonitor` of the zero Ref do nothing (`false`); zero values are equal
to each other and to no live value; `println` prints `<pid 0>` and
`<ref 0>`. Crash reports and `println` show only a PID's slot (`<pid
N>`, no generation): a restarted worker may reuse its predecessor's
slot, and the zero PID looks like the main actor (slot 0). `Whereis` of
an unknown name and a failed `port.Open` return the zero PID.

### 5.5.1 Per-actor memory budget

An idle actor's memory has three parts (32-bit targets):

| Part | Target | How |
|---|---|---|
| control block | 48 bytes | only pc, frame pointer, heap bounds and allocation pointer, mailbox head and tail, id, state and flags, memory limit, module slot, and a pointer to the extra record. Unlike AtomVM, no x registers, exception fields or process dictionary in the control block |
| heap + stack | about 96 bytes at start | registers live in stack frames inside the actor's block, not in the control block. The initial size is the entry function's frame + the closure environment + some slack (the image gives each function's frame size); it grows on demand |
| extra record | 0 unless used | links, monitors, pending exits and the charge of timers live in a separate record allocated on demand (`struct ActorExtra`, `runtime/vm/signals.c`); registered names (`runtime/vm/names.c`) and timers (`Vm.timers`) are in global tables. Most actors pay nothing |

Rules that go with it:
- **Shrink when idle.** When the GC finds a block mostly empty (live data
  under a quarter of it), it copies into a smaller block (not below the
  minimum), so an idle actor shrinks back, much like Erlang's hibernate
  but automatic.
- **No hidden per-actor overhead.** The run queue and mailboxes are
  intrusive lists through the control block and the message blocks; no
  extra nodes are allocated. Message blocks count against the receiver.
- **Allocator.** The VM has its own small-block allocator (size classes
  with free lists, `runtime/vm/alloc.c`) to avoid the 8-16 byte header
  per block of a platform malloc.
- **Observable.** `actor.Info(pid).Memory` returns the actor's bytes
  (control block + table slot + block + charged words: queued messages,
  pending `SendAfter`s, links and monitors), comparable to AtomVM's
  `process_info(memory)`. `tests/footprint` checks the idle-actor budget.

### 5.5.2 Memory layout (32-bit)

**Terms** (low-bit tags, `runtime/vm/term.h`):

```
xxxxxxxx xxxxxxxx xxxxxxxx xxxxxxx1   small int, 31-bit signed (boxed beyond)
oooooooo oooooooo oooooooo oooooo00   heap ref: word offset within the actor's block (not a pointer)
iiiiiiii iiiiiiii iiiiiiii iiii0010   special: nil, false, true, empty string, zero Ref, nil slice, empty Bytes
iiiiiiii iiiiiiii iiiiiiii iiii0110   PID: 16-bit slot + 12-bit generation (slot 0 gen 0 = zero PID)
gggggggg sssooooo oooooooo oooo1010   const ref: module tag (8-bit generation + 3-bit slot) + 17-bit const-area word offset (§5.5.3)
iiiiiiii iiiiiiii iiiiiiii iiii1110   off-heap data handle: index into the VM-wide reference-counted table (frozen Bytes)
```

Heap references are offsets from the start of the block, so a block can
move as a whole (GC, growth, shrinking), and merging a message block into
a heap only adds one constant to every reference in it.

### 5.5.3 Module tags (hot loading)

The VM holds at most `GOGO_MAX_MODULES` (8 by default, at most 8)
modules in its module table `Vm.mods` (`runtime/vm/module.h`). The boot
image is module 0 and takes the same load and start path as a hot-loaded
module: `vm_boot` installs the image in slot 0 (the requirement check
included), and `vm_start` starts it with `module_spawn` through its export
`main` (without one it reports `gogo: image has no main` and exits
non-zero). Module 0 differs only in that its code is in flash, it does
not count against the code-memory limit, and the VM holds it forever.
Each actor records its module slot (the control block's `mod` byte);
actors spawned with `actor.Spawn` inherit it.

Every term or raw word that names something in a module's image carries
the **module tag** in its top 11 bits: bits 31-24 are the low 8 bits of
the slot's generation, bits 23-21 the slot.

```
const ref          gggggggg sss  17-bit const-area word offset    1010
closure word 1     gggggggg sss  21-bit function index
struct word 1      gggggggg sss  00000  16-bit type index        (a module's own type, index >= TYPE_OWN_BASE = 128)
                   00000000 000  00000  16-bit type index        (reserved types 0..TYPE_RESERVED_COUNT-1: global, untagged)
```

The five bits between the slot and a struct's type index are reserved
for per-type flags (pointers, a later phase) and 0 in every v15 image
(see the reserved term encodings under the image format, §5.6).

- **Images hold no tags.** The compiler emits `offset << 4 | 0xA` as
  before, so the image format is unchanged except that offsets are
  limited to 17 bits (at most 2^17 words = 512 KB of constants per image;
  the ESP image partition is 1 MB), function indexes to 2^21 and type
  indexes to 2^16 (`TYPEIS`'s basic types start at `0x10000`). Both the
  loader and the compiler check this (`IMG_ERR_BOUNDS` / a compile
  error), and constant refs inside constant objects and the type words
  of constant structs must be untagged.
- **The VM adds the module's tag where an image word becomes a run-time
  term:** `LOADK` (constant refs), `MKCLOSURE` (function index),
  `NEWSTRUCT` (a module's own type index), a crash's function name, and
  fields read out of constant objects (the container's tag). `TYPEIS`
  tags the type ids of its own image before comparing, so equal indexes
  in two modules are different types, while reserved types are the same
  in every module.
- **Module 0's tag is 0.** Slot 0 is never unloaded and its generation
  stays 0, so the main program's terms equal the image's bit for bit.
  This is not a special case but the uniform rule (every slot starts at
  generation 0 and increments on unload).
- **Every access through a tag checks the module table.** Constant refs
  resolve through `const_obj_of` (NULL when the slot is empty or the
  generation differs; the caller then ends the actor with `badcode`) and
  only to object starts in that module's constant map. Closures may be
  called or spawned only by actors of their own module (`CALLV` and
  `Spawn` require the tag to match the current actor's module; code never
  runs across modules). `GETFIELD` and `TYPEIS` require a struct's type to
  be reserved, the actor's own module's, or another module's that is
  still loaded; otherwise `badcode`. Comparisons (`EQ`/`NE`), `TYPEIS` and
  `println` (struct fields and slice elements included) that meet a value
  of an unloaded module (a constant ref, closure or struct) are `badcode`
  too, rather than "not equal", "not this type" or `?`: they share one
  check, `term_live`. `println`, port commands and `port.Open` apply it to
  the whole value, to the depth `println` prints and for at most 65536
  terms (`WALK_BUDGET`); a port command or `port.Open` whose value needs
  more is `badcode` as well.
- **Alias bound.** A tag holds 8 generation bits, so a stale term
  resolves again (to whatever module then occupies the slot) only after
  256 more loads into the **same slot**. `module_load` picks free slots
  round-robin (starting after the last slot used), which spreads reloads
  over the 7 non-boot slots: with all 7 free it takes about 7 x 256 =
  1800 loads. Even then the result is memory-safe (a constant ref must
  land on an object start of the new module; a closure runs only through
  the new module's checked function table), but it is the wrong module's
  data or code rather than `badcode`. An exact solution would scan every
  actor and invalidate matching tags before a generation wraps; it is not
  done.
- **Why tag at run time rather than relocate at load:** an image in flash
  or in another actor's `Bytes` cannot be rewritten; tagging once per
  `LOADK` by the actor's module serves the main program (tag 0) and
  hot-loaded modules with the same code. The cost: one test per `LOADK`
  of a constant, one table lookup per constant access (`const_obj_of` is
  no longer inline), and one comparison more in `GETFIELD` and `TYPEIS`.
  Measured on Unix (Apple M series): `fib(30)` runs about 3.5 % more
  instructions and takes about 4 % more time; loops heavy in constant
  strings and structs run about 6 % more instructions in about the same
  time. Hot loading as a whole added about 5.3 KB to the VM core.
- **Module table entries** (`ModEntry`, 64 bytes on 32-bit, allocated with
  `va_alloc` at load and freed at unload; `Vm` holds only 8 pointers, 8
  generations, the code bytes in use and the round-robin start): the
  validated `Image` (48 bytes; the interpreter reads it in place, so it is
  kept whole; the code memory is the image itself, its size derived from
  the header), the `Bytes` it lives in (one reference held), the actor
  slots of up to 4 holders (2 bytes each, cleared when the actor exits,
  so they are invalid before the slot is reused), the number of live
  actors and flags (held by the VM / counted in the limit). The
  generation lives only in `Vm.mod_gen`. When an actor is freed
  (`actor_free` -> `module_actor_exit`), its module's live count drops
  and its holds end; when neither remains, the module is unloaded: the
  generation increments and the `Bytes` reference and the entry are
  released. A `code.Module` handle is `slot << 16 | generation` (16
  generation bits, less prone to collisions than the 8 in terms).

**Actor control block** (48 bytes on 32-bit targets; `runtime/vm/actor.h`
is authoritative):

```
offset size field
 0     4    next         run queue / free list link (intrusive)
 4     4    mem          start of the memory block
 8     2    mem_words    block size in words (at most 64K words = 256 KB per actor)
10     2    hp           heap top (word offset, grows up)
12     2    sp           stack top (word offset, grows down)
14     2    fp           current frame (word offset)
16     4    pc           bytecode position (in flash)
20     4    mbox_head    message list head
24     4    mbox_tail    message list tail
28     4    extra        extra record, NULL for most actors
32     4    id           PID (slot + generation)
36     1    state        RUNNABLE / WAITING / EXITED
37     1    flags        trap_exit, has_extra, ...
38     2    reductions   reductions left in this time slice
40     2    mbox_words   total size of queued messages (charged)
42     2    max_words    memory limit (0 = default)
44     1    mod          module slot whose code it runs (counted in that module's live actors)
45     3    pad          unused; keeps 48 bytes on 32-bit
```

There is also a global actor table, 4 bytes per slot (a pointer to the
control block); a PID's slot indexes it, and the generation stops stale
PIDs from reaching a new actor.

**Memory block** (heap and stack share one block):

```
mem[0]                                                        mem[mem_words]
+--------------------------+--------------+-----------------------------+
| heap objects ...    -> hp |     free     | sp <- ... frame n ... frame 0 |
+--------------------------+--------------+-----------------------------+
         grows up                                   grows down
hp meets sp -> GC (Cheney copy into a new block, growing or shrinking it)
```

**Stack frame** (the registers live here, not in the control block):

```
high  +--------------+
      | ret_pc       |  a small int (code offset; -1 for an entry frame)
      | saved_fp     |  a small int (top-relative; 0 for an entry frame)
      | r(n-1)       |
      | ...          |  the register count comes from the compiler
      | r1           |  (the frame size in the image's function table)
low   | r0           | <- fp
      +--------------+
```

Every word on the stack is a valid term (the return address and the
saved frame pointer are small ints), so the GC scans the stack without
stack maps.

**Heap objects**: the first word is a header `[size: 26 bits | kind: 6
bits]`, followed by the contents (`runtime/vm/term.h`).

```
K_STRUCT   [hdr][type id][field0][field1]...     struct (type id for type switches)
K_STRING   [hdr][len][bytes...]                  string built at run time
K_INT      [hdr][raw32]                          boxed int
K_CLOSURE  [hdr][func index][cap0][cap1]...      closure
K_REF      [hdr][serial lo][serial hi]           actor.Ref
K_INT64    [hdr][lo][hi]                         boxed int64
K_SLICE    [hdr][off][len][cap][array]           slice header (array: K_ARRAY or K_BYTEARR)
K_ARRAY    [hdr][n][e0][e1]...                   slice backing store (terms), mutable
K_BYTEARR  [hdr][nbytes][bytes...]               []byte backing store, mutable, not scanned
K_BYTES    [hdr][nbytes][bytes...]               bytes.Bytes on the heap, immutable, not scanned
K_BUF      [hdr][len][array]                     bytes.Buffer
K_FORWARD  (written during GC)                   forwarding mark
```

**Message block** (off the heap, from the VM's small-block allocator):

```
+------+-------+-------+------+-------------------------+
| next | words | flags | root | copied objects ...      |
+------+-------+-------+------+-------------------------+
  4B     2B      2B      4B     refs are relative to the block
```

`Receive` copies the block to the receiver's `hp`, adds `hp` to every
reference in it and frees the block. A message that is a plain value (a
small int, a PID: `root` is the value itself) has `words = 0` and no
objects.

**The extra record** (allocated only when used): `struct ActorExtra` in
`runtime/vm/signals.c`: a pending exit's reason code and function name,
links, monitors (of this actor), watching (monitors this actor set up),
and `charged_words` (the words of pending `SendAfter`s it created, its
monitors (entries at both ends plus a reserved `DownMsg`) and its side of
links (entry plus a reserved `ExitMsg`), which count against its limit
together with `mbox_words` through `actor_charged_words`).

**An idle actor** (a typical `for { switch m := actor.Receive().(type) {
... } }` loop):

| Item | Bytes |
|---|---|
| control block | 48 |
| actor table slot | 4 |
| memory block: entry frame (2 + 4 registers) + closure environment (about 4 words) + slack | about 96 |
| extra record | 0 |
| **total** | **about 148** (target 160 or less) |

### 5.6 Bytecode and interpreter

- The per-opcode reference (every opcode, builtin, reserved type and the
  image file layout as tables) is [bytecode.md](bytecode.md), generated
  from `runtime/vm/opcodes.h`; this section is the narrative.
- Register-based bytecode with fixed 32-bit instruction words (`op | a<<8
  | b<<16 | c<<24`: an 8-bit opcode and three 8-bit operands), some
  followed by one extension word (`runtime/vm/opcodes.h`, which the Go
  compiler parses too). Each function has a fixed number of registers
  (its frame size, computed by the compiler).
- The interpreter is a plain `switch` loop with one case per opcode, no
  macro-generated code.
- **Image layout** (`runtime/vm/image.h`, little-endian words): an
  8-word header (magic, version, n_funcs, n_types, consts_words,
  code_words, entry, n_reserved), the function table (3 words each), the
  type table (2 words each: field count, name), the constant area, the
  code, and the constant map: one bit per constant word, set where an
  object starts (ceil(consts_words / 32) words, read in place from flash,
  no RAM). After the map come the requirements (`n_requires`, then one
  constant offset per name: a driver or a mount prefix, §5.4.4) and the
  export table (`n_exports`, then one `{name constant offset, function
  index}` pair per export). Every image is a module: `main` is exported as
  `"main"`, and exported `func()` / `func(arg any)` functions of package
  `main` are exported too. The loader checks that requirement and export
  names are constant strings at object starts and rejects out-of-range
  function indexes. Images are position-independent and run in place.
- **Types.** The type table holds `n_reserved` reserved rows (header word
  7), then the image's own struct types. Code names a reserved type by its
  fixed index (`TYPE_*`) and an own type by `TYPE_OWN_BASE` (128) plus its
  position among the own types, whatever the reserved count; the table row
  is `t < 128 ? t : t - 128 + n_reserved`. The compiler emits reserved rows
  only up to the highest one the program uses, so an image built by a newer
  compiler runs on an older VM unless it uses a newer type. A VM refuses
  `n_reserved` above its own `TYPE_RESERVED_COUNT` as a newer format.
- **Append-only reserved fields.** A reserved type may gain fields only at
  the end, of type int, where 0 means the default (`RepeatMs` 0 means 50,
  `BatchMs` 0 means 200, and so on). The loader compares each reserved row's
  field count with its own (`TYPE_NFIELDS`, a packed table in
  `runtime/vm/opcodes.h`) and refuses a larger one as a newer format;
  `GETFIELD` past the end of a reserved struct from an older image reads
  int 0 (past the end of an own struct it is still `badcode`); drivers read
  field `i` only when `i < n`. A field is never reordered or removed: a
  deprecated one is renamed `_`.
- **Image validation.** The loader checks the structure of every table
  and of the constant area: the map must mark exactly the object starts
  that the constant headers define, and constant refs inside constant
  objects, function names and type names must point to marked starts,
  otherwise the image is refused (`IMG_ERR_BOUNDS`). `LOADK`'s constant
  operand is checked against the map when executed, not at load (a jump
  may land on an extension word, so a linear decode at load cannot see
  every operand). A ref into the middle of an object could forge one (a
  fake struct header inside a string's bytes, say), which would be a
  memory-safety bug, not just `badcode`. Function indexes, type ids and
  jump targets are checked when used (out of range is `badcode`).
- **Image format version** (`IMAGE_MAJOR`.`IMAGE_MINOR`: format **15**,
  the current minor in `runtime/vm/image.h`):
  header word 1 is `major << 16 | minor`. A VM runs the images of its own
  major whose minor is at most its own (one unsigned compare); any other
  is refused (`IMG_ERR_VERSION`: `GOGO_EVERSION` in the embedding API,
  `code.BadVersion` from `code.Load`, and gogovm, the firmware and the
  embed layer name both formats, `unsupported image format 15.1 (this VM
  runs 15.0 to 15.0)`). The compiler stamps each image with the lowest
  minor its program needs: the highest minor among the opcodes, builtins
  and reserved rows it uses, from tables in `compiler/image.go` (everything
  in 15.0 except the additions listed there, such as `screen.Config`).
  The versioning policy:
  - **Minor (additive):** a new builtin number, opcode, reserved type, or
    an int field appended to a reserved type (0 means the default). Old
    images keep running; a new image runs on an old VM unless it uses the
    addition.
  - **Major:** a change to the term encoding, to opcode or builtin
    semantics, or a reserved field reordered or removed. v15 is planned to
    be the last major for a long time; the single-number versions 1 to 14
    before it are listed at the top of `runtime/vm/image.h`.
  - `gogo version` and the device's boot banner print the format
    (`image format 15.<minor>`).
  - **Reserved term encodings.** So that interfaces, pointers and maps
    (the language phases still to come) need only a minor, v15 keeps
    encodings free for them (`runtime/vm/term.h`): heap kinds 12-15, the
    special immediates from `0x72` up, the `TYPEIS` ids `0x10007` to
    `0x100FF` beside `TY_*`, and bits 16-20 of a struct's type word
    (always 0 in a v15 image). Nothing in a v15 compiler or VM produces
    them; `image_load` refuses a constant object of an unknown kind but
    does not check the others. What keeps them from an older VM is the
    version check: an image that uses one is stamped with the minor that
    gave it a meaning, so an older VM refuses it rather than misreading
    it.
- **Builtins** (`B_*` in `runtime/vm/opcodes.h`, called with
  `OP_BUILTIN`): 0-27 actors and ports (`println`, `Spawn`, ... ,
  `port.Open` 26, `port.Close` 27), 28-45 strings, slices, `Bytes` and
  `Buffer`, 46 `port.Handoff`, 47 `net.Subscribe`, 48 `sys.Config`, 49
  `B_FS` (gogo/fs), 50 `B_CODE` (gogo/code), 51-53 `sys.Time`,
  `sys.SetTime`, `sys.Random`.
- **Reserved types** (`TYPE_*` in `runtime/vm/opcodes.h`, 45 of them,
  0..44): 0-5 `gogo/actor` (`ProcInfo`, `Reason`, `ExitMsg`, `DownMsg`,
  `Request`, `Options`), 6-8 `gpio.Config`, `gpio.Write`, `gpio.Edge`,
  9-22 `gogo/net`, 23-24 `fs.Info`, `fs.Space`, 25-29 `gogo/kbd` and
  `gogo/power`, 30 `code.Module`, 31-34 `port.Chunk`, `port.Out`,
  `port.Done`, `port.Closed` (the stream protocol), 35-36 `gogo/imu`,
  37-38 `gogo/ir`, 39-40 `gogo/audio`, 41 `net.DialTLS`, 42-43
  `port.Host`, `port.Post` (embed mode), 44 `screen.Config` (15.1). The
  15.0 table (0..43) is frozen by `TestMinorBaseline` in `compiler/`.

### 5.7 Scheduling

- One scheduler with a FIFO run queue.
- Every call and every backward jump costs one reduction; when the
  budget (1000 per slice) is used up the actor is switched out, so an
  endless loop is preempted too.
- With no runnable actor and no event source reporting pending work, the
  scheduler computes the nearest deadline (timers and event sources) and
  waits on the platform (`platform_wait`: a FreeRTOS notification on the
  ESP, `poll` on Unix) until an interrupt or event wakes it.
- The run loop is split (`runtime/vm/interp.h`): `vm_start` starts
  `main`, `vm_step` runs scheduler passes and reports runnable, idle or
  done, `vm_next_deadline` gives the nearest deadline and `vm_stop`
  closes the ports. `vm_run` loops over them; an embedding host may
  call them itself, waiting with the same latched wakeup.

### 5.8 Errors and crashes

- A run-time error (index out of range, division by zero, a write to
  read-only memory, exceeding the memory limit, ...) ends only the
  current actor.
- **Exit reasons** are `actor.Reason{Code int; Func string}`. `Code` is
  `actor.Normal` (the entry function returned), `Killed`, `OutOfMemory`,
  `MailboxOverflow`, `NoProc`, `DivZero`, `BadCode`, `Shutdown`,
  `BadShift`, `BadMsg`, a library failure (64..99: `actor.Failed` (64)
  plus a shared error code, such as `http.ListenFailed`; 10..63 are kept
  for the VM), or a user code (100 or more, from
  `actor.Exit(code)` / `actor.Kill(p, code)`); `Func` is the name of the
  function where a run-time error happened, else empty.
- On exit, linked actors get an exit signal (an `ExitMsg` if they trap
  exits, otherwise they exit too unless the reason is `Normal`), and
  monitors get a `DownMsg`. The message blocks for both notifications are
  reserved when the link or monitor is set up, so exit allocates nothing
  and delivery is guaranteed; the reserved memory counts against the
  limit of the actor that set it up. `Kill(p, Killed)` cannot be
  trapped.
- Crash reports go to stderr: `gogo: actor <pid N> crashed: <kind>[ in
  <func>]`, where `<kind>` is `killed`, `oom`, `mailbox_overflow`,
  `noproc`, `divzero`, `badcode`, `badshift`, `badmsg` or `exit(<code>)`;
  `Normal` and `Shutdown` are not reported. There are no line numbers
  yet.
- **Error codes** are one space, `gogo/errors` (`errors.Code`; the C
  side's `GOGO_E_*` in `runtime/include/gogo_errors.h`, which a test keeps
  equal): 0 is `OK`, 1..35 are meanings every package shares
  (`NotFound`, `Exists`, `NoMemory`, ... `BadVersion` = 27; 28..35 are
  free), capped so that `actor.Failed` (64) + a shared code stays a
  library exit reason below the user codes from 100; 36..99 are reserved,
  and each package that needs its own codes gets a block of 20 from 100
  (`gogo/http` 100..119, `gogo/sound` 120..139). The codes are
  compile-time constants: no opcode, builtin, reserved type or image
  field depends on the table (the C side uses the same names). A later
  language phase gives `Code` an `Error()` method and makes functions
  return `error`. **Open for that phase:** whether `fs.Err`, `code.Err`
  and the other result types become aliases of `errors.Code`, or the
  language gets an `errors.Is`-like rule. Until one of them, an `fs.Err`
  and an `errors.Code` of the same number are different types: they do
  not compare today, and as Go `error` values with different dynamic
  types they would not be equal either. Equal numbers make either step
  possible; they do not by themselves make `err == errors.NotFound` work
  across packages.
- Supervision is the `gogo/supervisor` library, written in the subset:
  `Supervise(child, spec)` and `SuperviseModule(m, entry, arg, spec)`
  supervise one child each (restart types Permanent and Transient, a
  maximum restart intensity), and `Upgrade` replaces a module's child.
  There is no multi-child strategy yet.

### 5.9 Toolchain and workflow

```
gogo build ./app -o app.ggb           # compile the .gg files of ./app
gogo run app.ggb                      # run on the host VM (simulated peripherals)
gogo dis app.ggb                      # disassemble
gogo flash app.ggb                    # write the image to the board's image partition (needs esptool)
gogo monitor -reset -timeout 30 -until "vm exited"   # capture the serial output
```

`gogo` with no arguments prints its usage. `gogo run` builds a directory
to a temporary image, runs `$GOGO_VM` or `<repo>/build/gogovm` and exits
with its status: 0 when `main` exited Normal, 1 for a usage, toolchain or
build error, 2 for a bad image or version, 3 for a missing requirement, 4
when `main` crashed, 64..99 for a library failure of `main` and 100..255
for `main`'s own code (book chapter 12). The VM core does not keep main's
exit reason: `platform/unix/main.c` reads it from the crash report the VM
writes on stream 2 for every abnormal exit, matching main's PID while the
actor is still in its slot, so the status costs no VM bytes.

On an ESP32-S3 the firmware and the image are stored separately: the
firmware is built with ESP-IDF and flashed once, and programs change
without reflashing it:

```
. ~/esp/esp-idf/export.sh
idf.py -C platform/esp32s3 build flash         # firmware (VM + platform + screen console)
gogo flash app.ggb                             # image, into the gogo partition at 0x210000 (header + CRC)
gogo monitor [-port P] [-timeout seconds] [-until text] [-reset]
```

`gogo flash` uses the only `/dev/cu.usbmodem*` by default. `gogo monitor`
sets the serial port to raw, CLOCAL and no HUP on its own descriptor, and
resets the chip only with `-reset` (DTR/RTS are the reset and boot
pins). Only one program can open the serial port at a time. Boot
messages and the partition table are described in
[platform/esp32s3/README.md](../platform/esp32s3/README.md).

**Device configuration (`gogo config`).** `gogo config set|get|rm|list
<key> [value]`; `gogo device scan` (the 2.4 GHz networks the board sees),
`gogo device stats` (network counters, heap) and `gogo device
format-flash` use the same channel (`gogo config scan|stats` still work,
deprecated). A value of `-` reads one line from
stdin (`gogo config set wifi.password -` keeps the password out of the
shell history). On the device the store is the firmware's NVS
(namespace `gogo`; reflashing the firmware or the image does not clear
it), reached over the USB serial port with `\x02gogo-config ...` frames;
`\x02` in a program's own output is written as `?`, so a program cannot
forge a reply. `wifi.` keys are invisible to `sys.Config`, and `get
wifi.password` echoes only `(set)`. Store selection on the host: `-port`
names a board; `-local` the local file `$GOGO_CONFIG` (default
`~/.gogo/config`); with neither, a set `$GOGO_CONFIG` means the local
file, otherwise exactly one connected board is used (with a note on
stderr), otherwise the local file.

**Pushing files (`gogo push`).** The same frames carry a file to
`/flash`: `push <path> <len> <crc32>`, then `data <offset> <base64>`
frames of 128 bytes each, each answered (`ok <bytes so far>`) before the
next is sent, so the board's 256-byte console input buffer never
overflows, then `end`. The console task writes a temporary file beside
the target (`fs_lfs.c`; LittleFS calls take a mutex, since the VM task
uses the file system too), reads it back, checks the length and CRC-32,
and renames it over the target, atomically. A failure, an `abort` or a
new `push` drops the temporary file. While a push erases or compacts a
flash block, a `/flash` call of the VM waits for the mutex, tens of ms
at worst (no audio frames were dropped during a push on the M5StickS3). `set` validates against a key table
in `cmd/gogo/config.go` (name, type: string, size, ip, bool or int,
platform-owned or a library's): keys are 1-15 bytes of `[a-z0-9._]`,
an unknown key under a reserved prefix (`wifi.`, `net.`, `fs.`,
`code.`, `gogo.`) is refused, and a known key's value must parse as its
type, as its reader parses it (`fs_lfs.c`, `module.c`, `gogo/dhcp`). The
firmware and the VM still parse what they read and fall back to a
default on a bad value; the table only moves the error to the moment the
value is typed. `gogo config keys` prints it.

### 5.10 Tests

- **Golden output tests** (`tests/golden/`, `tests/golden_test.go`): the
  output of example programs under the host VM is compared with expected
  output committed in the repository. Programs make their output depend
  only on the causal order of messages (for example through one printing
  actor), not on scheduling details. This is the main check of the
  compiler and VM semantics. Board examples with an `expected.out`
  (`examples/`) run the same way.
- Compiler: positive and negative tests of the subset check, code
  generation and image tests (`compiler/*_test.go`).
- VM: C unit tests per module (`runtime/*/test/`), built for the host and
  for the MCU configuration (`make test`), plus a 32-bit compile check
  (`make check32`).
- Network: scenarios against a peer simulator over a socket link with a
  simulated clock (`tests/net/`), TLS included.
- **Memory footprint test**: creates N idle actors and checks
  `actor.Info().Memory` against the 160-byte target (warning) and the
  256-byte hard limit (failure) (`tests/footprint`).
- Size: `make vmsize` measures the VM core for the ESP32-S3 (-Os) and
  counts its lines of C; `tools/vmsize.sh --record` appends both to
  `docs/vmsize-history.csv` (the `lines` column is empty in rows recorded
  before it existed).

### 5.11 Runtime architecture: instances, host and board tables

Since embed mode the runtime is a library of instances: every piece of
run-time state belongs to one `Vm`, and the VM reaches the outside world
only through two tables of function pointers that the caller hands it.

- **Per-instance state.** `struct Vm` (`runtime/vm/actor.h`) holds the
  actor table, run queue, timers, names, the `Bytes` handle table, the
  emergency reserve, the wall-clock offset, the fallback PRNG state, the
  module table, the allocator's free lists (`VaHeap`), the port table and
  the event ring with its indices. Subsystems that only some builds link
  hang off it as pointers to state the caller supplies: `vm->vfs`
  (`VfsState`: mounts and open files), `vm->input` (`InputState`: kbd,
  power, IMU, IR, audio), `vm->net` (`NetState`: the whole MCU IP stack,
  with the TLS connection inside it). `runtime/` has no mutable globals
  and no weak symbols; `tools/globals.sh` (run by `make globals` and on
  `libgogo.a`) fails the build on any `.bss`, `.data` or common symbol
  not on its allow-list. Board code under `platform/` may keep
  singletons (the ESP firmware's one `Vm`, its `NetState`, the GPIO pin
  slots). Field order matters on the ESP32-S3: the hook pointers and hot
  scalars come first, within the short-load range, and the large arrays
  (`ports[]`, the event ring) last.
- **The host table** (`gogo_host`, public in `gogo.h`): what every host
  gives: output, memory chunks, the monotonic clock, wall clock, random,
  `wait`/`wake` (a latched wakeup: a wake before or during a wait ends
  it), config lookup, module code memory, and (embed only) `destroy`, run
  once with the host's context after the VM's last reference. Hooks a
  host may omit are `NULL`; the wrappers in `runtime/vm/host.c`
  (`vm_now`, `vm_out`, `vm_alloc_chunk`, ...) are the only callers and
  apply the defaults. A hook that can fail returns 0 or a negative
  `GOGO_E*` (`set_unix_time_ms`; `host.c` turns that into the VM's own
  1/0). The table starts with its `size` and grows only at the end;
  every table is written with designated initializers (`.write =
  host_write`), never positionally. The ESP firmware's table is
  `esp_host` (`platform_esp.c`), the unix VM's is in `platform/unix/`,
  and `gogo_host_posix()` is the one that `libgogo` ships. The VM core
  reads a board's table in place and does not look at `size`;
  `gogo_new` copies an embedder's table into the `gogo_vm` (the first
  `size` bytes, the rest `NULL`), so the embedder's copy may go at once.
- **The board table** (`VmBoard`, internal, `runtime/vm/host.h`): board-only
  hardware: the port drivers (each generic driver an instance over the
  board's hardware layer, §5.11.1), the network interface (`NetIf`), the
  TLS port's hooks (`TlsHw`: allocator, clock, trust anchors, random) and
  the file systems it serves (`VmMount`: `/flash`, `/sd`, each with its
  backend). The port and mount lists are also what the board provides: an
  image's requirements are checked against them, so a board builds both
  with `#if` from what its build has. An embedded VM has no board
  hardware: its drivers are the host ports registered before
  `gogo_load`, and its one mount is `/flash` when `fs_root` serves it;
  `gogo_config.grant` can withhold names.
- **Time.** `vm->t0` is the host clock when the VM starts, and `vm_now`
  counts from it, so every VM starts near 0 whatever the host clock reads
  (the ESP's `esp_timer` has been running since boot). Code that gets a
  host timestamp (an IR frame's start, the GPIO debounce) converts with
  `t0`.
- **Interrupts and other threads.** Every producer names its instance.
  The event ring stays single-producer, single-consumer and ISR-safe; the
  ESP GPIO ISR finds its `Vm *` in the pin's slot and pushes with
  `evq_push(vm, ...)`. The Wi-Fi task hands frames and link changes to
  the stack it feeds, `net_rx_offer(NetState *, ...)` and
  `net_link_event(NetState *, ...)` (a single-slot handoff, not the
  ring). Both then wake that VM through its host's `wake`.
- **Start-up order.** `vm_init` (tables, limits), `vm_boot` (the
  image becomes module 0, requirements checked), then the board attaches its
  subsystems to the instance (`net_install`, `vfs_init`,
  `input_install`), then `vm_run`. Both boards go through one function
  for this, `gogo_board_main` (§5.11.1). Wi-Fi frames or link events that
  arrive before `net_install` are dropped; the first link-up and DHCP
  after boot work unchanged (checked on the StickS3 in E12).
- **Run loop.** `vm_run` is a loop over `vm_step` (scheduler passes until
  runnable, idle or done) and `vm_next_deadline`, waiting in `host->wait`
  when idle. A host that drives the VM itself (a game loop, an event loop)
  calls the same pair through `gogo_step` / `gogo_next_deadline`;
  `gogo_step` returns idle only after a final check that the event ring
  and the post inbox are empty, so a post that races the check is never
  slept through.
- **The embed layer** (`runtime/embed/`): `gogo_new` / `gogo_load` /
  `gogo_start` / `gogo_step` / `gogo_run` / `gogo_free` around a `Vm`;
  `gogo_post` (lock-free multi-producer inbox, delivered as `port.Post`
  to a registered actor) and `gogo_close_posts`; for other threads a
  `gogo_poster` (`gogo_poster_post`, `_port_post`, `_close_posts`,
  `_free`): a small block from the host's `alloc` holding a reference to
  the `gogo_vm` and a state word (a use after the free is undefined
  behaviour, which a debug build often catches), so a posting thread may outlive
  the owner's `gogo_free` (posts then return
  `GOGO_ECLOSED`; the memory and the host's `destroy` wait for the last
  reference); host ports (`gogo_register_port`, `gogo_port_emit`,
  `gogo_port_close`, which delivers every queued post and then a
  `port.Closed` made at the open and queued outside the owner's quota, so
  it is never lost; a `gogo_port`
  is an opaque `uint32_t`) opened from Go as `port.Host`, registered
  before `gogo_load` so they
  satisfy the image's requirements (`gogo_config.grant`, a list of names
  or NULL for all, can withhold them); a sandboxed `/flash` on a host directory
  (`fs_dir.c`, `gogo_config.fs_root`, per-instance `fs_quota`); and the
  POSIX host (`host_posix.c`, `gogo_run_thread`, declared apart in
  `gogo_posix.h` so that `gogo.h` stays free of threads). Every call that takes a
  `gogo_vm` runs on the thread that owns the VM; the poster's calls run
  on any thread, so the two handle types carry the threading rule (in
  Rust, `Send` and `Send + Sync`). A post and `gogo_free` meet in a
  store-buffering handshake (the poster counts itself in with seq_cst,
  then reads the state; the free raises the state, then waits for the
  count to drain), so either the post sees the close or the free waits
  for it. Names are a pointer and a length throughout. Several VMs in one
  process share nothing.
- **`libgogo`.** `make lib` builds `build/libgogo.a`: the VM core, vfs,
  stream and the embed layer, strict C99 with `-DGOGO_EMBED`, no board,
  input, net or TLS code. The objects are prelinked into one relocatable
  object whose only globals are the `gogo_*` functions of `gogo.h`
  (`tools/libsyms.sh` checks it); `libgogo-debug.a` is the same objects
  unprelinked. The book's chapter 18 (Embedding gogo) is the user guide;
  `examples/embed-c` is the smallest host.
- **Cost on the boards** (E12, measured against the pre-embed tree): VM
  core +644 B (`tools/vmsize.sh`), firmware +1.9 to +2.9 KB, static
  internal RAM +256 to +320 B, of which 196 B is the `Vm *` in each of
  the 49 GPIO pin slots; console output and the board smoke programs are
  unchanged.

### 5.11.1 The platform layer: driver instances and hardware layers

A board does not copy runtime code: every generic driver lives once in
`runtime/`, and the board describes its hardware to it in small tables
of functions, each with a `ctx` its hooks get first (so one set of hooks
can serve two devices, and the table can be a board's `static const`).

- **Driver instances.** `PortDriver.hw` (`runtime/vm/port.h`) is the
  instance's hardware layer; the VM never reads it. A board lists
  `GPIO_DRIVER(&my_gpio)`, `INPUT_KBD_DRIVER(&my_kbd)`, ... among its
  ports, and the driver finds its table at `p->drv->hw` in every
  callback, and through the open port in the event source's polls. An
  absent device is a driver the board does not list (`port.Open` fails,
  and an image that requires it is refused).
- **GPIO** (`runtime/gpio/gpio.c`, `GpioHal`): `npins`, a bitmap of
  reserved pins, the per-pin slots (`GpioPin`, board storage: `runtime/`
  keeps no state of its own) and `config` / `write` / `read` / `release`
  (+ optional `live`). Validation, the reserved pins, the edge match and
  the single-edge bounce filter are in the runtime once; the board's
  interrupt handler (or the unix stdin reader) reads the level and calls
  `gpio_edge(hal, pin, level, now_ms)`, which pushes to the event ring
  of the VM that opened the pin and returns whether to wake it.
  `config` may lower the debounce (unix script mode: 0) and refuses a
  pin the chip cannot use in that mode.
- **Input** (`runtime/input/input.h`): `KbdHal`, `PowerHal`, `ImuHal`,
  `IrTxHal`, `IrRxHal`, `MicHal`, `SpkHal`, one per device, each with a
  `ctx` and, where events can stop coming, an optional `live`. The
  drivers' logic (key mapping, auto-repeat, battery averaging, batching,
  streams) is unchanged; `InputState` still holds one port of each kind
  per VM.
- **Storage** (`runtime/vfs/lfs/`, `runtime/vfs/fat/`): LittleFS over a
  `BlockDev` (`read` / `prog` / `erase` / `sync` on blocks, its
  geometry), with the mount policy (first-boot format, grow in place,
  refuse a shrink or a file system larger than the device) in
  `lfs_be_mount`; FatFs over a `DiskHal` (`attach` at two speeds,
  `check`, sector `read` / `write`, `sectors`), with the lazy mount and
  remount in the backend and FatFs's disk layer as `fat_be_disk_*` for
  the board's FatFs glue. The volume state (`LfsVol`, `FatVol`) is board
  storage. Both are compiled by the firmware build only: the repo does
  not carry LittleFS, and FatFs only as the ESP component.
- **Mounts** (`VmMount{prefix, be, ctx}` in `VmBoard.mounts`):
  `vfs_init` mounts every entry with a backend; an entry without one
  still satisfies a requirement (unix without `--fs-root`, the embed
  layer's `/flash`, which `gogo_new` mounts itself). A volume that failed
  to come up stays listed and answers `NotMounted`.
- **Configuration** (`runtime/configsvc/`): the `\x02gogo-config`
  protocol (`gogo config`, `gogo device`, `gogo push`) once, over a
  board's `KvHal` (`get` / `set` / `rm` / `each`), its console output
  (whole lines), its `/flash` backend for push, and optional board
  requests (`scan`, `stats`, `format-flash`). The board's console reader
  feeds it bytes; `configsvc_vm_get` is `sys.Config`'s lookup (no
  `wifi.` keys). The ESP's store is NVS (`config_esp.c`). The unix VM has
  no console protocol: `gogo config` edits its file directly.
- **One boot path** (`runtime/board/board_main.c`):
  `gogo_board_main(board, host, run, words, nwords, &run_rc)` does
  `vm_init`, `vm_boot`, `net_install`, `vfs_init`, `input_install` in
  that order and runs the VM, for the ESP firmware and the unix VM alike.
  The board's state (the `Vm`, `VfsState`, `InputState`, `NetState`) is
  in the `BoardRun` it fills, with hooks for its own steps (unix: the
  wall clock after `vm_init`, its own run loop for exit statuses; ESP:
  the Wi-Fi heap report before the run); `net_install` is a pointer
  there, so a build without the network links none of it. The platform
  reports a boot error its own way (unix exit statuses, the ESP console
  line and idle).
- **Not libgogo.** The boards do not boot through the embedding API.
  `gogo.h` is the stable surface for hosts that bring only a `gogo_host`
  and host ports; a board needs what that API hides on purpose (the
  `VmBoard` with its driver instances and mounts, the net stack's
  `NetState`, the input source, a `Vm` in static storage) and would have
  to widen it to reach them. libgogo stays dogfooded by its own tests and
  `examples/embed-c`; `gogo_board_main` is the boards' shared path
  instead.
- **Atomics.** The VM core uses only a fence (`vm_barrier`); keep
  compare-and-swap out of it. The embed layer's post inbox and
  `embed_set_state` use `__atomic` compare-and-swap, which a Cortex-M0+
  (RP2040: no LDREX/STREX) does not have across its two cores; libgogo
  on such a chip needs a lock hook (a hardware spinlock) in place of
  those operations. Not implemented.

**What a new board writes** (an RP2040 board, say; about 600 lines and
no runtime copies):

1. A `gogo_host`: output, memory, `now_ms` (`time_us_64() / 1000`),
   `wait` / `wake` (WFE / SEV: the latched wake), `random`
   (`get_rand_32`), `config_get` (`configsvc_vm_get` over its `KvHal`).
2. A `GpioHal`: `npins` 30, the reserved bitmap (flash QSPI, the
   board's own pins), `GpioPin pins[30]` in static storage, `config` /
   `write` / `read` / `release` over the SDK, and an edge interrupt that
   calls `gpio_edge` and wakes the VM; listed as `GPIO_DRIVER(&hal)`.
3. Storage: a `BlockDev` over the flash region after the image (erase
   and program run from RAM with interrupts off, the other core parked),
   a static `LfsVol`, and at boot `lfs_be_mount`; its `VmMount{"/flash",
   &lfs_be_backend, &vol}` in the board's mounts. With an SD slot: a
   `DiskHal` over the SPI card, a static `FatVol`, FatFs's `disk_*`
   calling `fat_be_disk_*`, and `VmMount{"/sd", &fat_be_backend, &vol}`.
4. Input devices it has: one `*Hal` each and its `INPUT_*_DRIVER`.
5. A `NetIf` for the CYW43 on a Pico W (net/netif.h), optionally `TlsHw`.
6. Configuration: a `KvHal` over a flash sector, a static `ConfigSvc`,
   and a console reader that feeds it (`configsvc_feed`).
7. Its `VmBoard` (designated initializers), a `main` that maps the image
   and calls `gogo_board_main` with a static `Vm` and its states, and
   the build lines for `runtime/vm`, `runtime/board`,
   `runtime/gpio`, `runtime/vfs` (+ `lfs/`), `runtime/input`,
   `runtime/stream`, `runtime/configsvc` and `runtime/net`.

### 5.12 Versioning and stability

- **Releases** follow semantic versioning for the toolchain and firmware
  together. While at 0.x a minor release may break things, only with a
  "Breaking" section in its notes and a mechanical rewrite tool where one
  is possible; patch releases never break anything. 1.0 freezes the stable
  sets below.
- **Image format:** `major.minor` in header word 1 (§5.6). A VM runs the
  images of its major up to its own minor, and the compiler stamps the
  lowest minor a program needs. A new builtin, opcode, reserved type or
  appended int field (0 the default) is a minor; a term-encoding change, a
  change in opcode or builtin semantics, or a reserved field reordered or
  removed is a major (a deprecated field is renamed `_`, never removed).
  Language phases still to come are planned so that at most one more major
  is needed. Release notes state "firmware X runs images of format M.0 to
  M.m; compiler Y emits M.k"; `gogo version` and the boot banner print the
  format.
- **Requirements** are names (§5.4.4), so a new driver needs no format
  change: an image that names it is simply refused by a firmware without
  it.
- **C ABI** (`runtime/include/gogo.h`): within an ABI version functions
  keep their signatures and meaning and new ones are additive; the structs
  a host fills (`gogo_host`, `gogo_config`) carry a leading `size`, set by
  their `GOGO_*_INIT`, and grow only at the end: the library reads the
  fields it knows, an older struct's missing fields take their defaults,
  and a newer one's extra fields are ignored, so a field may be appended
  only if its 0 keeps the old behaviour; a size below the v0.2 struct's
  and a nonzero reserved word are refused (`GOGO_EINVAL`), so no size
  ends inside a field and the reserved words stay free to use; in the structs the library fills
  (`gogo_stats`, `gogo_image_info`) it writes back in `size` how much it
  filled, and every field within that is written, 0 on an error; anything marked experimental is
  outside these guarantees (`GOGO_EXPERIMENTAL` on the declaration, a
  macro that is empty unless the host makes it a deprecation; the book's
  18.12 has the table: host ports, `grant`, `fs_root`, `fs_quota`, the
  code-load flag (the `flags` field itself is stable), stats, image inspection, `gogo_run_thread`);
  `gogo.h` is C99 without threads and `gogo_posix.h` adds the POSIX host
  and `gogo_run_thread` (a split of the headers only: the prelinked
  `libgogo.a` still needs pthreads, `getentropy`, `clock_gettime` and the
  `*at` file calls); `libgogo.a` exports only `GOGO_API` symbols,
  the reviewed list in `runtime/include/libgogo.syms`
  (`tools/libsyms.sh`). `gogo_version()` gives the library's version and
  `gogo_image_format()` the image formats it loads; `gogo_image_info_get`
  and `gogo_image_requires` read an image's format and requirement names
  without a VM. The API's error
  results are the shared error codes negated (`GOGO_EINVAL` is
  `-GOGO_E_BADARG`), so one table describes them (`gogo_strerror`); only
  `GOGO_ESTATE` (-36) is the API's own, from the range 36..99 that is
  never a shared code.
- **Library API:** stable at v0.2: `actor`, `bytes`, `port` (its generic
  part), `fs`, `sys`, `strings`, `strconv`, `kv`. Experimental: `net`,
  `http`, `dns`, `dhcp`, `sntp`, `code`, `supervisor`, `imu`, `ir`,
  `audio`, `sound`, `wav`, `imulog`, `ringlog`, `irproto`. A deprecated
  name stays for one minor release with a compiler warning.

## 6. Status and roadmap

**Done:** actors and the VM (links, monitors, timers, `Call`, names,
supervisors, per-actor limits, strings, slices and `Bytes`); networking
(Ethernet to TCP, DHCP, DNS, the HTTP client and server, Wi-Fi and
`gogo config`); storage (`/flash`, `/sd`, `kv`, `ringlog`); input
(keyboard and battery); hot loading; optional firmware features; IMU and
infrared; audio; the wall clock and SNTP; HTTPS; the text screen
(`screen` port, `gogo/tui`); image format 15 with append-only reserved
types and named requirements (§5.6); one stream protocol for every byte
port with `port.Closed` and `From`; embed mode phase 1 (`libgogo`,
§5.11); user packages with a `GOGOPATH` search and `#!` scripts; the
first language additions (expression `switch`, labels, `range` over int,
variadic functions, methods with value receivers).

**Next, in order:**
1. **API v0.2:** shared error codes, a versioned C ABI (`size`-first
   structs, a poster handle), CLI exit codes and commands, and a platform
   layer (driver instances with hardware hooks, portable LittleFS/FAT)
   that makes new boards cheap.
2. **Distributed actors:** remote PIDs and a `gogo/node` library; boards
   connect to a server node that also relays board-to-board messages,
   with direct LAN links as a later step.
3. **Language completeness:** pointer receivers, interfaces and
   `error`, `panic`, pointers, maps, read-only package variables, `fmt`,
   sized integers, `float32`/`float64`, arrays, embedded fields, static
   `defer`.
4. **Hardware:** a `usb` port (the serial stream and connect events), the
   Arduino Nano ESP32, RP2040 boards, Bluetooth (BLE).
5. **Embedding bindings:** Rust, Go (with an in-process compiler,
   `libgogoc`), iOS and Android.

Not planned: a JIT; USB device classes (HID, mass storage).

## 7. Design decisions

1. Built-in packages and libraries live under the `gogo/` import path and
   never pose as Go standard library paths.
2. `int` is always 32 bits, type-checked as 32 bits at compile time; run
   time overflow wraps.
3. Zero-copy large data goes only through `bytes.Buffer.Freeze()` ->
   `bytes.Bytes`.
4. Message copies preserve sharing and cycles; PIDs, Refs, ports and
   frozen `Bytes` pass as they are; closures can be sent.
5. Queued messages count against the receiver's limit, and a receiver
   over its limit exits; when the whole machine runs out of memory the
   allocating actor is killed, and the VM keeps an emergency reserve.
6. Exit reasons are integer codes plus a function name
   (`actor.Reason`), not error values.
7. Tests are mainly golden outputs, and example output depends only on
   the causal order of messages.
8. Per-actor budget: control block 48 bytes, initial heap and stack
   about 96 bytes, extra record on demand; idle target 160 bytes, hard
   limit 256 bytes on 32-bit, checked by a test.
9. Dependencies: the VM is plain C99 on libc and POSIX (board platform
   layers may use the chip SDK; TLS uses mbedTLS), built with a Makefile
   on Unix, with its own test framework; the compiler is Go with only the
   standard library.
