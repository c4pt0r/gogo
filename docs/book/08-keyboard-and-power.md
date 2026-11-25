# 8. Keyboard and Power

Two more ports give programs the keyboard and the battery: `"kbd"` (package
`gogo/kbd`) and `"power"` (package `gogo/power`). They work like the GPIO
port of the last chapter: open them with `port.Open`, receive typed events
in your type switch, and they close when your actor exits.

Both are the same on every board and on the host. A board without the
hardware simply refuses to open the port (`ok == false`): the M5StickS3 has
no keyboard, so `port.Open("kbd", ...)` fails there. Write programs that
check `ok` and carry on with what the board has.

Each of these ports has **one owner at a time** in the whole VM; a second
`Open` fails while the first is open. If several actors need key presses,
have the owner forward them.

## 8.1 The keyboard

```go
package kbd

type Config struct{ RepeatDelayMs, RepeatMs int }

type Key struct {
	Code, Char, Mods int
	Down             bool
}
```

Open the port with `port.Open("kbd", kbd.Config{RepeatDelayMs: 400,
RepeatMs: 60})`. `RepeatDelayMs` is how long a key must be held before it
repeats (0 turns repeat off); `RepeatMs` is the interval between repeats
(0 or less means 50).

Every press, repeat and release is a `kbd.Key` message:

- **`Code`** says which key: its USB HID usage code, the same on every
  board. Letters are 0x04 to 0x1D, digits 0x1E to 0x27; the package has
  constants for the rest: `kbd.Enter`, `Esc`, `Backspace`, `Tab`, `Space`,
  `Delete`, `Up`, `Down`, `Left`, `Right`, `F1` to `F12`, and the modifier
  keys `LeftCtrl`, `LeftShift`, `LeftAlt`, `Opt` and `Fn` (0xF0, which has
  no HID code).
- **`Char`** is the ASCII character the key types with the current
  modifiers (US layout), or 0. Shift gives the shifted character; Ctrl with
  a letter gives a control character (Ctrl-C is 3); Enter is 10, Tab 9,
  Backspace 8, Esc 27, Delete 127.
- **`Mods`** is a bit set of the modifiers held: `kbd.ModCtrl`,
  `ModShift`, `ModAlt`, `ModOpt`, `ModFn`.
- **`Down`** is true for a press or a repeat, false for a release.

The Cardputer's keyboard is small, so the **Fn** key gives a second layer:
with Fn held, `;` is Up, `.` is Down, `,` is Left, `/` is Right and `` ` ``
is Esc, Backspace is Delete, the digit keys `1` to `0` are F1 to F10, and
`-` and `=` are F11 and F12. The event then carries the Fn
layer's `Code` and `Char`. A key without an Fn meaning reports its normal
code, with `ModFn` set. A release always carries the same `Code` as its
press, even if you let go of Fn first, so keys never get stuck.

Modifier keys produce their own `Key` events too (with `Char` 0). Only the
last non-modifier key held repeats. There is no Caps Lock.

Here is a small line editor:

```go
// A line editor on the Cardputer keyboard: type characters, Backspace
// deletes, Enter prints the line, Esc quits. On the host the keys come
// from the stdin script in "input".
package main

import (
	"gogo/actor"
	"gogo/kbd"
	"gogo/port"
)

// printable holds ASCII 32..126, so character c is printable[c-32 : c-31]
// (string([]byte{byte(c)}) works too, but allocates a slice per key).
const printable = " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~"

func main() {
	_, ok := port.Open("kbd", kbd.Config{RepeatDelayMs: 400, RepeatMs: 60})
	if !ok {
		println("no keyboard on this board")
		return
	}
	line := ""
	for {
		switch k := actor.Receive().(type) {
		case kbd.Key:
			if !k.Down {
				continue // ignore releases
			}
			if k.Code == kbd.Esc {
				println("bye")
				return
			} else if k.Code == kbd.Enter {
				println("line:", line)
				line = ""
			} else if k.Code == kbd.Backspace {
				if len(line) > 0 {
					line = line[:len(line)-1]
				}
			} else if k.Mods&kbd.ModCtrl != 0 && k.Char == 3 {
				println("^C: line cleared")
				line = ""
			} else if k.Char >= 32 && k.Char <= 126 {
				line = line + printable[k.Char-32:k.Char-31]
			}
		}
	}
}
```

On the host the keys come from a script on standard input, with lines
`key <name>` (press and release), `keydown <name>` and `keyup <name>`:

```text
key h
key i
key space
keydown shift
key g
keyup shift
key o
key x
key backspace
key enter
key a
key b
keydown ctrl
key c
keyup ctrl
key o
key k
key 1
key enter
key esc
```

```text
line: hi Go
^C: line cleared
line: ok1
bye
```

Key names in scripts are `a`-`z`, `0`-`9`, `enter`, `esc`, `backspace`,
`tab`, `space`, `delete`, `up`, `down`, `left`, `right`, `f1`-`f12`,
`ctrl`, `shift`, `alt`, `opt`, `fn` and the punctuation characters `- = [ ]
\ ; ' ` , . /`. Run `gogo run` in a terminal without a script and your
computer's keyboard becomes the Cardputer's keyboard: printable characters,
Enter, Backspace, Tab, Esc and the arrow keys are passed through.

`examples/kbddemo` in the gogo tree prints every event with its name and
modifiers; it is a handy way to see what each key sends.

A key event that would take the owner over its memory limit is dropped;
the owner is not killed. A key held when the port opens never got a press
event, so you may see a release without a press; ignore it.

> **Note.** The keyboard and battery have been checked on both boards
> (every key, the Fn layer, modifiers, fast typing, battery and charging
> readings). Auto-repeat has not been exercised on a board yet, and the
> Cardputer's battery reading has not been compared with the vendor
> firmware's; see [`docs/known-issues.md`](../known-issues.md).

## 8.2 The battery

```go
package power

type Config struct{ PeriodMs int }
type Battery struct{ MilliVolts, Percent, Charging, External int }
type Read struct{}
```

Open with `port.Open("power", power.Config{PeriodMs: 5000})`. `PeriodMs`
is how often the board samples the battery chip (0 or less means 10000,
the minimum is 1000). The owner receives a `power.Battery`:

- once when the port opens (the first good sample);
- after every `power.Read{}` command you send to the port;
- at once when `Charging` or `External` changes;
- when `Percent` has moved by 2 or more since the last one sent.

Nothing is sent while nothing changes; there is no heartbeat.

The fields:

- **`MilliVolts`**: the cell voltage, as the average of the last 4 samples.
- **`Percent`**: 0 to 100, computed linearly from `MilliVolts`: 3300 mV or
  less is 0 %, 4100 mV or more is 100 %.
- **`Charging`**, **`External`**: 1 (yes), 0 (no) or -1 (the board cannot
  tell). On the M5StickS3 both are read from its power chip. The
  Cardputer ADV cannot see either and always reports -1.

```go
// Watch the battery. The power port sends a Battery message when it opens,
// on every Read, when charging or external power changes, and when the
// charge moves by 2 % or more. On the host the samples come from the
// "battery <mV> <charging> <external>" lines of the stdin script.
package main

import (
	"gogo/actor"
	"gogo/port"
	"gogo/power"
)

func main() {
	p, ok := port.Open("power", power.Config{PeriodMs: 5000})
	if !ok {
		println("no battery monitor on this board")
		return
	}
	actor.Send(p, power.Read{}) // ask for a reading now
	n := 0
	for n < 3 {
		switch b := actor.Receive().(type) {
		case power.Battery:
			n++
			state := "on battery"
			if b.External == 1 {
				state = "on external power"
			} else if b.External < 0 {
				state = "power source unknown"
			}
			if b.Charging == 1 {
				state = state + ", charging"
			}
			println(b.MilliVolts, "mV", b.Percent, "%", state)
			if b.Percent < 20 && b.External != 1 {
				println("  low battery!")
			}
		}
	}
}
```

The host's battery is simulated with `battery <mV> <charging> <external>`
lines:

```text
battery 4100 1 1
battery 4095 1 1
battery 4000 0 0
battery 3550 0 0
```

```text
4100 mV 100 % on external power, charging
4073 mV 96 % on battery
3936 mV 79 % on battery
```

Follow the numbers to see the rules at work. The first line arrived with
the `Read` (4100 mV, 100 %). The second line (4095 mV) moved the average
by too little to report. The third changed `Charging` and `External`, so
it was reported at once, and its voltage is the average of the samples so
far, 4073 mV, not 4000. The fourth moved the percentage enough to be
reported, again averaged.

> **Note.** The package documentation calls `MilliVolts` "the cell
> voltage", but it is a moving average of the last 4 samples (see
> `runtime/input/power.c`). After a sudden change it takes 4 samples to settle.

There is no power-button event: the StickS3's power chip handles its button
by itself (click to restart, double click to power off).
