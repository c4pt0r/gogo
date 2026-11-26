# 15. IMU and IR

Both boards have a six-axis motion sensor (an IMU: three accelerometer axes
and three gyroscope axes) and an infrared LED; the M5StickS3 also has an
infrared receiver. Three ports give programs this hardware: `"imu"`
(package `gogo/imu`), `"ir_tx"` and `"ir_rx"` (package `gogo/ir`), with
`gogo/irproto` for the common NEC remote-control protocol.

These ports move more data than a key press, so they use gogo's two
**stream** messages, defined in `gogo/port` and shared by every streaming
port:

```go
type Chunk struct {
	Seq, TimeMs, Dropped int
	Data                 bytes.Bytes
}
type Out struct{ Data bytes.Bytes }
type Done struct{ Seq int }
```

- An input stream sends the owner `port.Chunk` messages. `Seq` counts from
  1, including Chunks that were lost, so a gap in the numbers means
  something was dropped. `Dropped` is how many were lost since the last one
  that arrived. `TimeMs` is when the data began (the low 31 bits of the
  clock `actor.Now` reads).
- An output stream takes `port.Out` commands. Every `Out` is answered with
  one `port.Done`, in order: `Seq` numbers the Outs that were sent
  (1, 2, ...), and `Done{Seq: -1}` means this one was refused or could not
  be sent. A port holds at most **two Outs at a time** (one being sent, one
  waiting); a third is refused at once. Send the next `Out` when a `Done`
  comes back, and you never lose one. (One caveat: a refusal is itself a
`Done{-1}`, and when 16 Dones are already owed and unread, an owner that
reads nothing while someone floods it with Outs, further refusals are
dropped. Accepted Outs always get theirs.)

A stream never kills its owner: when the owner's mailbox is over its quota
a Chunk is dropped (and counted in the next one's `Dropped`), and a Done
waits until there is room.

Like the keyboard, each of these ports has one owner at a time, and a board
without the hardware refuses to open it. The firmware can also be built
without them (section 15.4); then `port.Open` fails the same way.

## 15.1 The motion sensor

```go
package imu

type Config struct{ RateHz, Gestures, BatchMs int }
type Gesture struct{ Kind, Value int }

const (
	Tap = 1; DoubleTap = 2; TripleTap = 4; Orientation = 8
	Flat = 16; AnyMotion = 32; NoMotion = 64; Step = 128
)
```

The port does two independent things: it **samples** the sensor, and it
reports **gestures** that the sensor recognises by itself.

**Samples.** `RateHz` is 25, 50, 100 or 200 samples per second, or 0 for
no samples at all (any other value makes `Open` fail). Samples are gathered
for `BatchMs` milliseconds (default 200, clamped to 50..1000) and arrive as
one `port.Chunk`. Each sample is 12 bytes, six little-endian `int16`
values:

    gx gy gz ax ay az

The ranges are fixed: acceleration is +-8 g over +-32768 (4096 per g), and
rotation +-2000 degrees per second over +-32768. The axes are the
sensor chip's own; they differ between the two boards, so a program that
cares about "up" should find gravity rather than assume an axis.

Here is a spirit level that averages each batch:

```go
// A spirit level: sample the motion sensor at 25 Hz in batches of 200 ms,
// average the acceleration of each batch and print it in milli-g, with
// the axis that gravity pulls on. On the host the samples come from the
// "imu sample gx gy gz ax ay az" lines of the stdin script.
package main

import (
	"gogo/actor"
	"gogo/imu"
	"gogo/port"
)

// s16 is the little-endian int16 at byte i of b.
func s16(b []byte, i int) int {
	v := int(b[i]) | int(b[i+1])<<8
	if v >= 32768 {
		v = v - 65536
	}
	return v
}

// mg converts a raw acceleration (+-8 g over +-32768) to milli-g.
func mg(raw int) int { return raw * 8000 / 32768 }

func abs(v int) int {
	if v < 0 {
		return -v
	}
	return v
}

func main() {
	_, ok := port.Open("imu", imu.Config{RateHz: 25, BatchMs: 200})
	if !ok {
		println("no motion sensor on this board")
		return
	}
	buf := make([]byte, 0, 240) // one batch: 200 ms at 25 Hz = 5 samples of 12 bytes
	for batches := 0; batches < 3; {
		switch c := actor.Receive().(type) {
		case port.Chunk:
			batches++
			n := c.Data.Len() / 12
			buf = buf[:c.Data.Len()]
			c.Data.CopyTo(buf)
			ax, ay, az := 0, 0, 0
			for i := 0; i < n; i++ {
				ax += s16(buf, 12*i+6) // a sample is gx gy gz ax ay az
				ay += s16(buf, 12*i+8)
				az += s16(buf, 12*i+10)
			}
			ax, ay, az = mg(ax/n), mg(ay/n), mg(az/n)
			down := "z"
			if abs(ax) > abs(ay) && abs(ax) > abs(az) {
				down = "x"
			} else if abs(ay) > abs(az) {
				down = "y"
			}
			println("batch", c.Seq, "samples", n, "accel mg", ax, ay, az, "gravity on", down)
		}
	}
}
```

On the host, with the script in `examples/15-imu-level/input`, it prints:

```
batch 1 samples 3 accel mg 9 -4 999 gravity on z
batch 2 samples 2 accel mg 1000 2 -6 gravity on x
batch 3 samples 1 accel mg -2 -1000 12 gravity on y
```

A board at rest shows about 1000 mg on one axis and close to 0 on the
others: that is gravity.

Three things to know about batches:

- **Memory.** The batch buffer is `BatchMs * RateHz * 12 / 1000` bytes (at
  most 2400) and is charged to the owner while the port is open, like any
  other memory it holds (chapter 6). 200 Hz with the default 200 ms is 480
  bytes; if the owner's limit cannot take it, `Open` fails.
- **TimeMs** of a Chunk is the start of its batch window (the open, or the
  previous Chunk), within one polling period (50 ms) of its first sample.
- **Closing discards the partial last batch.** To lose nothing, close the
  port right after a Chunk arrives.

**Gestures.** `Gestures` is a mask of the gesture kinds you want; each one
the sensor sees arrives as an `imu.Gesture` whose `Kind` is that bit. Two
kinds carry a `Value`:

- `Orientation`: the new orientation. The low two bits are 0 portrait
  upright, 1 landscape left, 2 portrait upside down, 3 landscape right; 4 is
  added when the board is face down.
- `Step`: the step count so far (cumulative; it is not reset by closing
  the port).

Gestures work with `RateHz: 0`, which keeps only the accelerometer
running. The sensor is polled about every 50 ms (sooner with a shorter
`BatchMs`), so a gesture reaches the program up to about 50 ms late, and
gestures seen in the same poll arrive in bit order (Tap first, Step last).
This example starts and stops a "recording" with a double tap:

```go
// Gestures only (RateHz 0: no samples): a double tap starts and stops a
// "recording", a change of orientation is reported, and the program ends
// after the step counter passes 10. On the host the gestures come from the
// "imu gesture <kind> [value]" lines of the stdin script.
package main

import (
	"gogo/actor"
	"gogo/imu"
	"gogo/port"
)

// side names the orientation number's low two bits.
func side(v int) string {
	if v == 0 {
		return "upright"
	}
	if v == 1 {
		return "on its left side"
	}
	if v == 2 {
		return "upside down"
	}
	return "on its right side"
}

func main() {
	_, ok := port.Open("imu", imu.Config{Gestures: imu.DoubleTap | imu.Orientation | imu.Step})
	if !ok {
		println("no motion sensor on this board")
		return
	}
	recording := false
	for {
		switch g := actor.Receive().(type) {
		case imu.Gesture:
			if g.Kind == imu.DoubleTap {
				recording = !recording
				if recording {
					println("double tap: recording")
				} else {
					println("double tap: stopped")
				}
			} else if g.Kind == imu.Orientation {
				face := "face up"
				if g.Value&4 != 0 {
					face = "face down"
				}
				println("orientation:", side(g.Value&3)+",", face)
			} else if g.Kind == imu.Step {
				println("steps:", g.Value)
				if g.Value > 10 {
					return
				}
			}
		}
	}
}
```

```
double tap: recording
orientation: on its left side, face up
steps: 4
double tap: stopped
orientation: upside down, face down
steps: 12
```

The script also has an `imu gesture tap` line; it is not printed because
`Tap` is not in the mask.

To record motion in the background, use `gogo/imulog`. It owns the port in
an actor of its own and appends every Chunk and every gesture to files, so
the rest of the program need not read them:

```go
l, ok := imulog.Start(imu.Config{RateHz: 50, Gestures: imu.Tap, BatchMs: 500}, "/flash/imu", 32768)
// ... later, or after a reboot:
for _, r := range imulog.Read("/flash/imu", 100) { // the newest 100, oldest first
    if r.Kind == imulog.ChunkRec { /* r.A Seq, r.B Dropped, r.TimeMs, r.Data samples */ }
    else { /* r.A gesture kind, r.B its value */ }
}
actor.Kill(l, actor.Shutdown) // stops the log and closes the port
```

The log is two files, `0.imu` (newest) and `1.imu`, rotated like `ringlog`
(chapter 9), so it never takes more than `maxBytes` plus one record. A
record is a 15-byte header plus the Chunk's data; `lib/imulog/imulog.gg`
documents the layout. Every record is synced, so a power loss costs at most
the last one. `examples/imulogdemo` shows it on the host.

## 15.2 Infrared

An infrared remote sends bursts of light flickering at a **carrier**
frequency (usually 38 kHz). gogo describes a frame as **timings**: a
`bytes.Bytes` of little-endian `uint16` microseconds, alternately "mark"
(carrier on) and "space" (off), starting with a mark. Each value is 1 to
32767 and a frame has at most 256 of them. The receiver produces exactly
the format the transmitter takes, so a frame can be learned and sent back
unchanged.

```go
package ir

type TX struct{ CarrierHz, DutyPct int }
type RX struct{ GapUs int }
```

**Sending.** `port.Open("ir_tx", ir.TX{CarrierHz: 38000, DutyPct: 33})`
opens the transmitter (0 means the defaults shown; the carrier may be
10000 to 500000 Hz, the duty at most 99 %). Send a frame with
`port.Out{Data: timings}`; its `Done` comes back when the frame has been
sent. An `Out` with bad timings or more than 256 values is refused with
`Done{Seq: -1}`. Closing the port forgets an `Out` still waiting (no
`Done`); a frame already being sent is finished if it ends within about
100 ms (an NEC frame does), else cut short.

**Receiving.** `port.Open("ir_rx", ir.RX{GapUs: 10000})` opens the
receiver. Silence longer than `GapUs` microseconds (default 10000, at most
32767) ends a frame, and each frame arrives as one `port.Chunk` whose
`Data` holds its timings, ending with a mark. A frame longer than 256
values is cut there and counts 1 in that Chunk's `Dropped`. `TimeMs` is
when the frame began. The receive buffer (512 bytes) is charged to the
owner while the port is open.

**NEC.** `gogo/irproto` builds and reads the NEC protocol that most
consumer remotes use: `irproto.EncodeNEC(addr, cmd)` returns the 67
timings of a frame, and `irproto.DecodeNEC(data)` returns `addr, cmd, ok`.
Anything else (air conditioners, for instance) is sent and received as raw
timings.

This program learns a button and plays it back:

```go
// Learn a remote control button and send it back. The first frame the
// receiver hears is kept; if it is an NEC code it is also decoded. Then the
// learned frame is sent twice, followed by an NEC code built from scratch.
// A transmitter takes at most two Outs at a time, so the third is sent
// when the first Done comes back. On the host the frames come from the
// "ir rx <us> ..." lines of the stdin script, and every frame sent prints
// "ir tx: <n> values" on stderr.
package main

import (
	"gogo/actor"
	"gogo/bytes"
	"gogo/ir"
	"gogo/irproto"
	"gogo/port"
)

func main() {
	rx, ok := port.Open("ir_rx", ir.RX{GapUs: 10000})
	if !ok {
		println("no IR receiver on this board")
		return
	}
	var learned bytes.Bytes
	for got := false; !got; {
		switch c := actor.Receive().(type) {
		case port.Chunk:
			learned = c.Data
			got = true
		}
	}
	port.Close(rx)
	println("learned a frame of", learned.Len()/2, "timings")
	addr, cmd, nec := irproto.DecodeNEC(learned)
	if nec {
		println("it is NEC: address", addr, "command", cmd)
	}

	tx, ok := port.Open("ir_tx", ir.TX{CarrierHz: 38000, DutyPct: 33})
	if !ok {
		println("no IR transmitter on this board")
		return
	}
	actor.Send(tx, port.Out{Data: learned})
	actor.Send(tx, port.Out{Data: learned})
	for sent := 0; sent < 3; {
		switch d := actor.Receive().(type) {
		case port.Done:
			sent++
			println("sent", d.Seq)
			if d.Seq == 1 {
				actor.Send(tx, port.Out{Data: irproto.EncodeNEC(4, 9)})
			}
		}
	}
}
```

```
learned a frame of 67 timings
it is NEC: address 4 command 8
sent 1
sent 2
sent 3
```

The learned frame came from a real remote, so its timings are not the
textbook ones (a 9630 us leader instead of 9000); `DecodeNEC` allows for
that, and sending it back reproduces what the remote sent. Keep the
receiver and the LED at least 30 cm apart when you try this on a board.

## 15.3 On the boards

| | M5StickS3 | Cardputer ADV |
|---|---|---|
| `imu` | BMI270 | BMI270 |
| `ir_tx` | LED on G46 | LED on G44 |
| `ir_rx` | receiver on G42 | none: `Open` fails |

- The motion sensor is a Bosch BMI270 on the board's internal I2C bus. Its
  tap, orientation, flat, motion and step detection run inside the chip
  (the firmware loads Bosch's 8 KB "legacy" feature configuration into it
  at the first `Open`, which takes about a quarter of a second). Neither
  board wires the sensor's interrupt to the processor, which is why the
  port polls.
- On the StickS3, the IR LED and receiver are powered from the 5 V output
  that also feeds the Grove and Hat connectors. The firmware turns it on
  while any IR port is open and off again when the last one closes (unless
  it was already on). **While an IR port is open, the Grove and Hat 5 V pins
  are outputs: do not feed 5 V into them then.** While `ir_rx` is open the
  speaker amplifier is held off, because the receiver does not work with it
  on: opening `"spk"` then fails, and a speaker already playing goes silent
  until `ir_rx` closes.
- The G42, G44 and G46 pins are reserved for IR: the `gpio` port refuses
  them.

## 15.4 Requirements and firmware

The compiler records which of these ports a program opens (`imu`, `ir_tx`,
`ir_rx`) in the image as requirements, as it does for the network and the
keyboard (chapter 14). A firmware built without them refuses such an image
at boot, naming the missing driver.
The switches are `GOGO_IMU` and `GOGO_IR` (`gogo firmware` names them
`imu`, `irtx` and `irrx`; one switch builds both IR ports). Turning
`GOGO_IR` off removes the IDF's RMT driver from the firmware; turning
`GOGO_IMU` off removes the sensor driver and its 8 KB configuration.

## 15.5 On the host

On the host VM the hardware is simulated from the stdin script, like the
keyboard and battery:

| line | effect |
|---|---|
| `imu sample gx gy gz ax ay az` | one sample (added to the current batch) |
| `imu gesture <kind> [value]` | one gesture: `tap doubletap tripletap orientation flat anymotion nomotion step` |
| `ir rx <us> <us> ...` | one received frame |
| `sleep <ms>` | wait before the next line |

Every frame the program sends prints `ir tx: <n> values` on stderr. Put a
`sleep` after sample lines that is at least 100 ms longer than `BatchMs`,
and after each `ir rx` line, so the output does not depend on timing.
