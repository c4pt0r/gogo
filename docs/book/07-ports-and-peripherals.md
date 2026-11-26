# 7. Ports and Peripherals

A microcontroller is there to talk to hardware. In gogo, every piece of
hardware an actor uses (a pin, the keyboard, the battery gauge, a file, a
network socket) is a **port**: a special actor, implemented in C by a
driver, that has a PID like any other actor.

- You **open** a port with `port.Open(driver, config)` and become its
  **owner**.
- Anyone holding the port's PID can **send it commands** with
  `actor.Send`, like any message.
- The port sends **events** (typed messages) to its owner only.
- The port **closes** when its owner exits, also when the owner crashes.

So hardware fits the actor model with no new concepts: you receive button
presses in the same type switch as everything else, and a crashed actor
cannot leave a pin claimed forever.

## 7.1 The port API

```go
package port

func Open(driver string, cfg any) (p actor.PID, ok bool)
func Close(p actor.PID)
func Handoff(p actor.PID, to actor.PID) (ok bool)
```

`Open` fails (`ok == false`, and `p` is the zero PID) if the driver is
unknown or not on this board, if the configuration is invalid, if the
resource is already taken, or if memory runs out. `Close` closes a port;
closing one that is already closed is ignored. `actor.Kill` on a port PID
also closes it.

Drivers and their configuration types:

| Driver | Config | Chapter |
|---|---|---|
| `"gpio"` | `port.GPIO` | this one |
| `"kbd"` | `kbd.Config` | 8 |
| `"power"` | `power.Config` | 8 |
| files | `fs.Open` (not `port.Open`) | 9 |
| `"netif"`, `"udp"`, `"tcp"`, `"tcp_listen"` | `net.IfConfig`, `net.UDP`, `net.Dial`, `net.Listen` | 10 |
| `"tls"` | `net.DialTLS` | 17 |
| `"imu"` | `imu.Config` | 15 |
| `"ir_tx"`, `"ir_rx"` | `ir.TX`, `ir.RX` | 15 |
| `"mic"`, `"spk"` | `audio.Mic`, `audio.Speaker` | 16 |
| `"screen"` | `screen.Config` | 19 |
| ports a host program registers | `port.Host` | 18 |

At most 16 ports can be open at once in the whole VM.

## 7.2 GPIO

A `"gpio"` port is one pin:

```go
type GPIO struct {
	Pin        int
	Mode       int // port.Input, InputPullUp, InputPullDown, Output
	Edge       int // port.None, Rising, Falling, Both (inputs only)
	DebounceMs int // input debounce; <= 0 means the default, 20 ms
}

type Write struct{ Value int }     // command: drive an output to 0 or 1
type Edge struct{ Pin, Value int } // event: the level changed; Value is the new level
```

### Output

Open the pin with `Mode: port.Output` and send it `port.Write` commands:

```go
// Drive an output pin: blink an LED three times. On the host a Write
// prints "gpio: pin N = V" on stderr instead of touching hardware.
package main

import (
	"gogo/actor"
	"gogo/port"
)

const ledPin = 5

func main() {
	led, ok := port.Open("gpio", port.GPIO{Pin: ledPin, Mode: port.Output})
	if !ok {
		println("cannot open the LED pin")
		return
	}
	for i := 0; i < 3; i++ {
		actor.Send(led, port.Write{Value: 1})
		actor.Sleep(100)
		actor.Send(led, port.Write{Value: 0})
		actor.Sleep(100)
	}
	port.Close(led)
	println("blinked 3 times")
}
```

On the host there is no LED, so the simulated driver reports each write on
standard error:

```text
gpio: pin 5 = 1
gpio: pin 5 = 0
gpio: pin 5 = 1
gpio: pin 5 = 0
gpio: pin 5 = 1
gpio: pin 5 = 0
```

Standard output:

```text
blinked 3 times
```

### Input and edges

An input pin reports level changes as `port.Edge` events, filtered by
`Edge`: `Rising` (0 to 1), `Falling` (1 to 0) or `Both`. The buttons on the
M5StickS3 (KEY1 on G11, KEY2 on G12) connect the pin to ground when
pressed, so they are opened with `InputPullUp` and `Falling`: one event per
press.

```go
// Count button presses. KEY1 on the M5StickS3 is G11, active low: open it
// with a pull-up and ask for falling edges. Each press arrives as a
// port.Edge message. On the host the presses come from the stdin script
// in the file "input" ("11 0" = pin 11 goes low).
package main

import (
	"gogo/actor"
	"gogo/port"
)

const key1 = 11

func main() {
	_, ok := port.Open("gpio", port.GPIO{Pin: key1, Mode: port.InputPullUp, Edge: port.Falling})
	if !ok {
		println("cannot open KEY1")
		return
	}
	presses := 0
	for presses < 3 {
		switch e := actor.Receive().(type) {
		case port.Edge:
			presses++
			println("press", presses, "pin", e.Pin, "level", e.Value)
		}
	}
	println("three presses, bye")
}
```

On the host, input comes from standard input. This example's `input` file:

```text
11 0
11 1
sleep 50
11 0
11 1
11 0
11 1
```

Run it with the script on standard input:

```text
$ gogo run docs/book/examples/07-button < docs/book/examples/07-button/input
```

```text
press 1 pin 11 level 0
press 2 pin 11 level 0
press 3 pin 11 level 0
three presses, bye
```

The script lines for GPIO are `<pin> <level>` and `sleep <ms>`. The VM
reads one line each time it has nothing else to do, so the output depends
only on the order of events, not on timing. After the end of the script
no more input can arrive, and the VM exits once the program is idle.

You can also drive pins from the keyboard: in a terminal, `gogo run --key
a=11 --key s=12 <dir>` turns a key press of `a` into a press of pin 11 (the
level goes to 0 and back to 1). End the input with Ctrl-D, or stop the
program with Ctrl-C.

### Debouncing

A mechanical button does not switch cleanly: one press can bounce between
0 and 1 several times within a few milliseconds. The GPIO driver treats an
edge that comes less than `DebounceMs` milliseconds (20 ms by default)
after the previous level change as bounce and drops it; on a single-edge
pin (`Rising` or `Falling`) this is what removes the bounce of a key
release. Set `DebounceMs` higher for noisy switches. In the host's script
mode debouncing is off, so scripts are exact; in the host's terminal mode
(`--key`) it is on.

### Which pins

Pins the board itself uses cannot be opened: on the M5StickS3 the USB
pins, the flash and PSRAM, the LCD and the power-management I2C bus; on
the Cardputer ADV the USB pins, flash, LCD, SD card, keyboard I2C bus and
interrupt and the battery ADC. `platform/esp32s3/README.md` lists them.
Free ones include KEY1 (G11) and KEY2 (G12) on the StickS3 and the G0
button on the Cardputer ADV (active low).

## 7.3 Owners, commands and events

**Commands** can come from any actor that has the PID. You can hand an
LED's PID to several actors and let each of them `Send` it `Write`s.

**Events** go only to the owner, as ordinary messages in its mailbox, and
count against its memory limit like any message. An owner that does not
read its events will eventually exit with `MailboxOverflow`.

You cannot link to or monitor a port; `SendAfter` and `Call` to a port are
ignored.

### Ports close with their owner

```go
// A port belongs to the actor that opened it, and closes when that actor
// exits, also when it crashes. So a restarted worker can open the same pin
// again: no leaked handles, no cleanup code.
package main

import (
	"gogo/actor"
	"gogo/port"
)

func worker(boss actor.PID, crash bool) {
	_, ok := port.Open("gpio", port.GPIO{Pin: 5, Mode: port.Output})
	actor.Send(boss, ok)
	if crash {
		zero := 0
		println(1 / zero)
	}
}

func main() {
	me := actor.Self()
	actor.Spawn(func() { worker(me, true) })
	println("first worker opened pin 5:", actor.Receive())
	actor.Sleep(10)
	actor.Spawn(func() { worker(me, false) })
	println("second worker opened pin 5:", actor.Receive())

	// While someone owns it, nobody else can open it.
	_, ok := port.Open("gpio", port.GPIO{Pin: 7, Mode: port.Output})
	_, again := port.Open("gpio", port.GPIO{Pin: 7, Mode: port.Output})
	println("pin 7 first open:", ok, "second open:", again)
}
```

```text
first worker opened pin 5: true
second worker opened pin 5: true
pin 7 first open: true second open: false
```

Standard error:

```text
gogo: actor <pid 1> crashed: divzero in worker
```

The first worker crashed while holding pin 5, and the VM closed the pin for
it, so the second worker could open it. This is what makes supervisor
restarts work with hardware: the restarted actor opens its pins again from
scratch. When the VM itself exits, it closes every port still open.

## 7.4 Handoff

Sometimes the actor that opens a port is not the one that should handle
its events. A web server's listener accepts connections and hands each to a
new actor; a setup actor opens the buttons and hands them to the UI actor.
`port.Handoff(p, to)` makes `to` the owner:

```go
// A port's events go to its owner. port.Handoff gives the port to another
// actor: main opens the button and hands it to a dedicated key handler.
package main

import (
	"gogo/actor"
	"gogo/port"
)

func keyHandler(boss actor.PID) {
	n := 0
	for n < 2 {
		switch e := actor.Receive().(type) {
		case port.Edge:
			n++
			println("handler: edge on pin", e.Pin)
		}
	}
	actor.Send(boss, "handler done")
}

func main() {
	btn, ok := port.Open("gpio", port.GPIO{Pin: 12, Mode: port.InputPullUp, Edge: port.Falling})
	println("open", ok)
	me := actor.Self()
	h := actor.Spawn(func() { keyHandler(me) })
	println("handoff", port.Handoff(btn, h))
	println("handoff again (main is no longer the owner)", port.Handoff(btn, me))
	println(actor.Receive())
}
```

`input`:

```text
12 0
12 1
12 0
12 1
```

```text
open true
handoff true
handoff again (main is no longer the owner) false
handler: edge on pin 12
handler: edge on pin 12
handler done
```

After a handoff, later events go to the new owner, the port closes when
*it* exits, and the memory charged for the port moves to it. Only the
current owner can hand a port off. `Handoff` returns false, and nothing
changes, if the caller is not the owner, the port is closed, `to` is not a
live actor, or `to` cannot take the port's memory charge within its limit.

## 7.5 Peripherals that are not ports

On the boards, everything a program prints (standard output in white,
standard error in red) appears on the LCD as a scrolling text console,
with Chinese characters supported. The console itself is not a port; the
`"screen"` port (chapter 19) takes the LCD over as a grid of character
cells while it is open, and gives it back to the console when it closes.
`uart`, `i2c` and `spi` ports are planned.

## 7.6 How ports work inside

You never need this section to use ports, but it explains why they behave
the way the earlier sections describe: why a slow actor cannot stall a
driver, why a crash frees everything, and why every peripheral looks the
same.

```
  owner actor                              VM (scheduler thread)                hardware / host
 ┌──────────────────┐  port.Open(name,cfg) ┌───────────────────────────────┐
 │                  │ ───────────────────► │ find driver by name           │
 │  p := PID        │ ◄─────────────────── │  board drivers, host ports    │
 │                  │      (PID, ok)       │ charge memory to the owner    │
 │                  │                      │ port table: slot + generation │
 │ actor.Send(p,cmd)│ ───────────────────► │ driver command                │ ──► registers,
 │                  │                      │                               │     frames, DMA
 │                  │                      │  ┌─────────────────────────┐  │
 │  receive event ◄─┼── mailbox (quota) ◄──┼──│ event delivery          │◄─┼─┐
 │  (typed struct)  │                      │  │ debounce, coalesce      │  │ │
 └──────────────────┘                      │  └─────────────────────────┘  │ │
                                           │      ▲                ▲       │ │
                                           │      │ drain          │ drain │ │
                                           │ ┌────┴──────┐  ┌──────┴─────┐ │ │
                                           │ │ event ring│  │ source     │ │ │
                                           │ │ (SPSC)    │  │ pending    │ │ │
                                           │ └────▲──────┘  │ deadline   │ │ │
                                           └──────┼─────────┴──────▲─────┴─┘ │
                                                  │ interrupt      │ poll    │
                                             gpio, kbd INT    net RX, IMU FIFO,
                                                              audio DMA, TLS
```

**Opening.** `port.Open` looks the driver up by name: first the board's
drivers, then any ports the host program registered (chapter 18). The
memory a port needs is charged to the caller; if the caller's limit cannot
hold the port's state plus one event, `Open` returns `ok == false` at once
rather than opening a port whose events would all be dropped. Each VM has
a table of 16 port slots. A port's PID is its slot number plus a
generation, so a PID kept after its port closed never reaches a later port
in the same slot.

**Drivers.** Each kind of peripheral is a C driver with a small set of
callbacks: open, command, close, and a few helpers for handoff and
backpressure. A driver turns commands into hardware actions and hardware
state into events. It never runs actors or allocates in their heaps.

**Two ways in for events.** A driver that is told about hardware by an
interrupt (a GPIO edge, the keyboard's INT line) pushes a small record
into an *event ring* from the interrupt handler and wakes the VM; the ring
has exactly one writer and one reader, so the handler never takes a lock
and never calls into the VM. A driver that has to look at hardware (the
network's received frames, the IMU's FIFO, audio DMA buffers, a TLS
handshake step) is an *event source* with three questions the scheduler
asks: is there work now, do the work, and when is the next time there
might be. The scheduler never sleeps while a source has work due, and work
that only depends on time waits for its deadline instead of spinning.

**Delivery and backpressure.** Events are typed structs (`kbd.Key`,
`imu.Gesture`, `port.Chunk`, ...) delivered to the owner's mailbox under
its mailbox quota. When the owner falls behind, a driver drops or merges
events and counts what it dropped (a debounced pin keeps only its latest
level; a `Chunk` reports `Dropped`), so a slow actor costs itself data but
never stalls the driver or other actors.

**Streams.** Ports that move bytes over time share one protocol
(`port.Out`, `port.Done`, `port.Chunk`): writes are `Out{Data}` messages,
each answered with `Done{Seq}` when it has been sent or played; at most
two may be in flight and a third is answered at once with `Done{-1}`;
input arrives as `Chunk{Seq, TimeMs, Dropped, Data}`. IR, the microphone
and the speaker use it (chapters 15 and 16). The `"screen"` port (chapter
19) takes `Out`s and answers with `Done`s too, by a different rule: it
draws each `Out` at once, so none waits in flight; a `Done` its owner has
no room for is owed (up to 8, retried until delivered), and an `Out`
arriving with 8 owed is not drawn and gets `Done{-1}`. The network ports still use
their own `net.Write` / `net.Written` / `net.Data` messages (chapter 10).

**Ownership.** A port belongs to the actor that opened it until it is
handed off (section 7.4). When the owner exits or crashes, the VM closes
every port it owns and frees what they hold, which is why gogo programs
need no cleanup code for hardware.

**Requirements.** The compiler records which drivers a program opens by a
constant name, and an image that needs a driver the firmware lacks is
refused when it is loaded, not halfway through running. A library can
mark a driver it can live without with `//gogo:optional` (chapter 12); its
`Open` then simply fails on boards without that hardware.
