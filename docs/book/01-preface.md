# 1. Preface

## What gogo is

gogo is a small virtual machine for microcontrollers, plus a compiler for a
subset of Go. You write your program in Go syntax, the `gogo` tool on your
computer compiles it to a compact bytecode image, and the VM runs that image
on the board: today the M5StickS3 and the Cardputer ADV (both ESP32-S3), with
RP2040 boards planned. The same VM also runs on macOS and Linux, so you can
develop and test on your computer before you touch hardware.

A gogo program is not structured the way an ordinary Go program is. There
are no goroutines, no channels and no shared memory. Instead a program is a
set of **actors**, in the style of Erlang: small, isolated processes that
each have their own memory and talk to each other only by sending messages.
Peripherals (buttons, the keyboard, the battery gauge, files, network
sockets) appear as **ports**: special actors that you send commands to and
that send you typed event messages.

Here is the flavour of it. This actor counts button presses:

```go
for {
	switch m := actor.Receive().(type) {
	case port.Edge: // the button was pressed
		count++
	case actor.Request: // someone asks for the count
		actor.Reply(m, count)
	}
}
```

## Why it works the way it does

gogo makes a few choices that will feel unusual if you come from Go. They
all follow from one goal: **one failing part must never take the whole
device down.**

- **Isolation.** Every actor has its own heap. No pointer, slice or map can
  be shared between two actors, so no actor can corrupt another's data.
  There are no locks because there is nothing to lock.
- **Messages are copied.** `actor.Send` makes a deep copy of the message
  (slices, nested structs and all). The receiver gets its own copy. The
  one exception is frozen `bytes.Bytes`, which are immutable and so can be
  shared safely.
- **Per-actor memory limits.** Each actor has a memory limit. An actor that
  goes over it (with a runaway loop, a leak, or a flood of incoming
  messages) is stopped. Other actors keep running.
- **Let it crash.** Instead of defensive error handling everywhere, an actor
  that hits something unexpected (division by zero, an index out of range,
  bad input) simply exits. Other actors are told through **links** and
  **monitors**, and a **supervisor** restarts the failed actor in a clean
  state. Resources the actor held, such as open pins and files, are released
  automatically when it exits.
- **Small and readable.** The VM is plain C99 with no third-party libraries
  and a 64 KB code budget. An idle actor costs about 150 bytes, so a board
  with 160 KB for actors can hold a thousand of them. The whole system is
  meant to be small enough for one person to read.

## What gogo is good for

- Firmware for small devices built from several independent activities: a
  button handler, a sensor poller, a display updater, a little web server,
  a logger. Each becomes an actor, and a bug in one does not freeze the
  others.
- Devices that must keep running unattended: supervisors restart what
  fails.
- Learning and experimenting: the language, VM and network stack are small
  enough to study end to end, and every program also runs on your laptop.

## What gogo is not good for

- **Existing Go code.** gogo compiles a *subset* of Go, and the subset is
  strict: no maps, no pointers, no methods, no interfaces of your own, no
  floats, no `switch` on values, no package-level variables, no channels or
  goroutines. Most Go libraries will not compile. Chapter 3 lists exactly
  what is in and what is out.
- **Number crunching.** There is no floating point, and the VM is an
  interpreter.
- **Hard real-time work.** The scheduler is preemptive but not real-time,
  and storage calls stall all actors while they run (a flash erase takes
  tens of milliseconds).
- **General-purpose secure networking.** There is an HTTPS client (TLS 1.2,
  one connection at a time), but no TLS server, no client certificates
  and no HTTP/2.
- **Big data.** Each actor is limited to 16 KB by default on a board, and a
  single actor can never use more than 256 KB.

## Where gogo is going

This book describes what is on gogo's `main` branch.
The next steps, in order:

1. **Embed mode** (in progress): embedding the VM in other programs, the
   way Lua is embedded, for desktop and server programs, phone apps and
   test tools.
2. **Language completeness**: more of Go, such as methods, maps, arrays,
   type assertions and the expression `switch` (chapter 3 lists what is
   missing today).
3. **Bluetooth (BLE)**: talking to a phone app, and setting up Wi-Fi from
   the phone.

HTTPS (TLS, chapter 17) is done. RP2040 boards are planned, with the
board and the timing still open. The design document,
[`docs/design.md`](../design.md), describes how the VM works.

## How to read this book

The chapters build on each other:

| Chapter | What you learn |
|---|---|
| 2. Getting Started | Build the tools, run a program on your computer, flash a board |
| 3. Language Basics | The Go subset: what works, what does not, and what to do instead |
| 4. Actors | Spawn, Send, Receive, Call/Reply, names, timers |
| 5. Failure and Recovery | Exit reasons, links, monitors, supervisors, "let it crash" |
| 6. Memory Model | Per-actor heaps and limits, message copying, Bytes and Buffer |
| 7. Ports and Peripherals | GPIO, events, owners, Handoff |
| 8. Keyboard and Power | The `kbd` and `power` ports |
| 9. Storage | Files on `/flash` and `/sd`, `kv`, `ringlog` |
| 10. Networking | DHCP, DNS, UDP, TCP, HTTP client and server, the date (SNTP) |
| 11. Configuration and Devices | `gogo config`, Wi-Fi, the two boards |
| 12. Toolchain Reference | Every `gogo` command |
| 13. Appendices | Library quick reference, exit reasons, error codes, limits, differences from Go, capabilities |
| 14. Hot Loading | Loading code at run time: `gogo/code`, modules, supervised upgrades |
| 15. IMU and IR | The motion sensor, infrared send and receive, streams |
| 16. Audio | The microphone and speaker, WAV files |
| 17. HTTPS | `https://` URLs, the clock, keep-alive `http.Client`, streaming uploads, the `"tls"` port |

Every example in the book is a complete program in
[`docs/book/examples/`](examples/), and the output shown is the real output
of running it on the host VM. You can run all of them with
`docs/book/examples/run.sh`, which also checks the output against the
book. Where the behaviour is likely to surprise you, the book says so in
a **Note** box; the open limitations are collected in
[`docs/known-issues.md`](../known-issues.md).

You should know some Go, or at least one C-like language. You do not need
to know Erlang or embedded programming; the book explains the ideas as they
come up.
