# 23. Bluetooth LE

Both boards have a Bluetooth Low Energy radio. The `"ble"` port makes the
board a **peripheral**: a small GATT server that a phone, a tablet or a
computer connects to. The program declares what the server offers when it
opens the port (services, each a set of characteristics), and from then
on a phone can read values the program has set, write values the program
receives as messages, and subscribe to notifications the program pushes.

That is enough for the usual reason to put Bluetooth on a device: a phone
app that talks to it, to read its status, list what it recorded, or tell
it to upload something. Setting up the board's Wi-Fi from a phone is a
separate thing, done by the firmware itself, not by a program; while it
runs, the port cannot open.

The port lives in package `gogo/ble`, a small library over `gogo/port`.
Its messages are the generic host-port ones (`port.Post` for events and
commands, chapter 22), so it needs no new image format: a program that
imports `gogo/ble` runs on every firmware of its format that has the
`ble` driver, which is both boards' default firmware.

## 23.1 Declaring a server

```go
p, err := ble.Open(ble.Config{
	Name: "gogo-demo",
	Services: []ble.Service{
		{UUID: ble.UUID16(0x180F), Chars: []ble.Characteristic{
			{UUID: ble.UUID16(0x2A19), Flags: ble.Read | ble.Notify},
		}},
		{UUID: ble.UUID("6e400001-b5a3-f393-e0a9-e50e24dcca9e"), Chars: []ble.Characteristic{
			{UUID: ble.UUID("6e400002-b5a3-f393-e0a9-e50e24dcca9e"), Flags: ble.Write | ble.WriteNoResp},
			{UUID: ble.UUID("6e400003-b5a3-f393-e0a9-e50e24dcca9e"), Flags: ble.Notify},
		}},
	},
})
```

- **`Name`** is what a phone's scan shows, 1 to 26 bytes (it travels in
  the 31-byte advertisement).
- A **service** is a UUID and 1 to 8 characteristics. A **characteristic**
  is a UUID and flags saying what a phone may do with it: `ble.Read`,
  `ble.Write` (with a response), `ble.WriteNoResp` (without one, faster)
  and `ble.Notify` (the phone may subscribe; the program pushes). At most
  4 services and 16 characteristics in all; more is `errors.TooMany`.
- A **UUID** is `ble.UUID16(n)` for the 16-bit ones the Bluetooth SIG
  assigns (0x180F is the Battery Service, 0x2A19 its Battery Level, which
  phone apps recognise), or the 36-character form for your own, written
  as a conversion: `ble.UUID("6e40...")`. Make up a 128-bit UUID for an
  app of your own (any UUID generator); the ones above are the "Nordic
  UART" service that many generic phone apps know.
- Characteristics are **numbered in declaration order**, across all
  services, from 0: in the example the battery level is 0, the command
  characteristic 1 and the reply characteristic 2. Every event and command
  names a characteristic by that number.

`Open` starts advertising at once and makes the caller the port's owner.
It fails with `errors.BadArg` for a Config it cannot use (an empty or long
name, a malformed UUID, a characteristic without flags, a service without
characteristics), `errors.NoMemory` on a board with too little internal
RAM to start Bluetooth (23.7), `errors.Busy` if the radio is taken (one `ble` port per
board; or the firmware's Wi-Fi setup from a phone, section 2.1, holds the
radio: it runs only at boot, before any program, so a program does not
meet it in practice), and
`errors.NotFound` on a firmware built without `ble`. On a board the open
waits while the Bluetooth controller and stack start: no actor runs
meanwhile.

One phone is connected at a time. While it is, the board does not
advertise, so no other central can connect; when it leaves, advertising
starts again by itself. Closing the port, or the owner's exit, ends the
connection and the advertising.

## 23.2 Events

The port's events arrive as `port.Post` messages from the port, and
`ble.Event` decodes one into a typed value:

```go
type Connected struct {
	Peer string
	MTU  int
}
type Disconnected struct{ Reason int }
type Written struct {
	Char int
	Data bytes.Bytes
}
type Subscribed struct {
	Char int
	On   bool
}
type MTUChanged struct{ MTU int }
type NotifyFailed struct {
	Char   int
	Reason errors.Code
}
```

- **`Connected`**: a phone connected. `Peer` is its address as the board
  sees it (phones show a random one that changes), `MTU` the ATT MTU so
  far: on a board always the default 23, since the phone's MTU request
  comes after the connection (`MTUChanged`); `gogovm`'s test phone may
  say another number.
- **`Disconnected`**: the phone went away. `Reason` is the Bluetooth
  reason code: 0x13 (19) the phone ended the connection, 0x08 the link
  timed out (the phone walked out of range), 0x16 the board ended it.
- **`Written`**: the phone wrote `Data` to characteristic `Char`, up to
  512 bytes. A write with a response is acknowledged to the phone when the
  driver has queued it, before the program reads it.
- **`Subscribed`**: the phone turned notifications of `Char` on or off.
  Every connection starts with all of them off.
- **`NotifyFailed`**: a `SendNotify` of `Char` was not delivered, and
  `Reason` says why; its `Done{Seq: -1}` comes right after (23.3).
- **`MTUChanged`**: the phone negotiated a larger ATT MTU; a notification
  may now carry `MTU - 3` bytes (`ble.MaxNotify(mtu)`). Phones usually ask
  right after connecting (iPhones and Macs for 185 or more, Android for up
  to 517); the board answers with at most 256, so a notification carries
  at most 253 bytes.

**A phone's read never reaches the program.** The driver answers it with
the value the program last set with `ble.SetValue` (empty until then), so
reading a status costs no message and works while the program is busy.
Set the value whenever it changes, and the first time right after `Open`
(a phone that connects in between reads an empty value):

```go
func SetValue(p actor.PID, char int, data bytes.Bytes)
```

`SetValue` sends no reply: a bad `Char`, more than 512 bytes, or a board
out of memory for the copy leaves the old value, silently.

Events the owner has no room for are held in the driver, not dropped, and
delivered when the mailbox has room again; meanwhile a phone's writes are
answered with an error, so the phone retries. A write without a response
that arrives while the driver's queue is full is lost (as it would be over
the air).

## 23.3 Notifications

```go
func SendNotify(p actor.PID, char int, data bytes.Bytes)
```

`SendNotify` pushes `data` to the phone, which must have subscribed to
`char`. Like a stream's `port.Out` (chapter 15), every one is answered
with a **`port.Done`** from the port:

- `Done{Seq: n}` once the Bluetooth stack has taken it (counting from 1);
- `Done{Seq: -1}` if it could not be delivered: refused (a characteristic
  without `Notify`, nobody subscribed, empty data, more than
  `MaxNotify(mtu)` bytes, two already in flight), or the phone left before
  it went out.

Every `Done{Seq: -1}` is announced by a **`NotifyFailed`** event just
before it, whose `Reason` says why:

| `Reason` | why |
|---|---|
| `errors.BadArg` | no such characteristic, one without `Notify`, empty data, more than `MaxNotify(mtu)` bytes |
| `errors.Closed` | no phone connected, or it went away while the notification waited |
| `errors.Refused` | the phone has not subscribed to that characteristic |
| `errors.TooMany` | two already in flight |
| `errors.IO` | the Bluetooth stack refused it |

`NotifyFailed` is a hint: it is dropped when the owner's mailbox is full,
while the `Done` never is. A refusal's `Done{-1}` comes at once, so it
may arrive before the `Done` of an earlier notification still in flight.

Two notifications may be in flight. When the stack has no buffer for one,
the driver waits and tries again, for as long as the connection lasts,
and the `Done` comes later: a sender that waits for each `Done` (or keeps
at most two in flight) never runs ahead of the radio. A notification is
never failed for being slow: a slow phone (a long connection interval,
an app in the background) only makes the `Done` late, and a link that
dies ends in a `Disconnected`, which fails every notification still
waiting with `errors.Closed`. Keep to two in flight: beyond that, a
refusal's `Done` can be lost when the mailbox is full. There is no hidden fragmentation: data longer than one notification
is the program's to split, as in this example
([`examples/23-ble-chunks`](examples/23-ble-chunks/main.gg)):

```go
// sendChunked pushes data to characteristic char in pieces of
// MaxNotify(mtu) bytes, one at a time; ok is false if one was not
// delivered (the phone unsubscribed or went away). The port.Posts that
// arrive meanwhile (a phone's write, a new MTU) come back in held, in
// order, for the caller to handle.
func sendChunked(p actor.PID, char int, data bytes.Bytes, mtu int) (pieces int, ok bool, held []port.Post) {
	size := ble.MaxNotify(mtu)
	for at := 0; at < data.Len(); at += size {
		end := at + size
		if end > data.Len() {
			end = data.Len()
		}
		ble.SendNotify(p, char, data.Slice(at, end))
		pieces++
		var seq int
		seq, held = waitDone(p, held)
		if seq < 0 {
			return pieces, false, held
		}
	}
	return pieces, true, held
}

// waitDone returns the Seq of the next port.Done from p. A port.Post that
// arrives first is appended to held, not dropped: it may be the phone's
// next write.
func waitDone(p actor.PID, held []port.Post) (int, []port.Post) {
	for {
		switch m := actor.Receive().(type) {
		case port.Done:
			if m.From == p {
				return m.Seq, held
			}
		case port.Post:
			held = append(held, m)
		}
	}
}
```

A `port.Post` that arrives while `waitDone` waits (the phone's next
write, a new MTU, a `Disconnected`) is kept in `held`, not dropped: the
main loop handles those before it receives again. Dropping them is an
easy mistake, and it loses a phone's write without a trace.

The program keeps the MTU from `Connected` and `MTUChanged`; with a
phone at the default MTU of 23 the 100-byte text goes in 5 pieces, at 50
in 3:

```text
advertising as gogo-log
mtu 23 -> 5 pieces of up to 20 bytes, ok: true
mtu 50 -> 3 pieces of up to 47 bytes, ok: true
phone gone
```

One notification at a time is the simplest; for throughput keep two in
flight (send the next on each `Done`, as `tests/ble/progs/echo` does), and
use the largest MTU the phone offers. `tools/ble_central.py` (23.6)
measures what a board reaches with a computer as the central.

How fast notifications go depends mostly on the phone (the central): it
picks the connection interval, and how many packets it takes in each
connection event. The board asks for 251-byte link-layer packets and a
15 ms interval when a central connects; a central may refuse. Measured on
a StickS3, 200 notifications each:

| Central | 20 bytes each | 253 bytes each |
|---|---|---|
| Raspberry Pi 5 (Linux, BlueZ): took 15 ms | 5 KB/s | 32 KB/s |
| macOS: kept 30 ms, skips most connection events | 0.4 KB/s | 1.2 KB/s |

So send few large notifications rather than many small ones, and do not
judge a phone app by a Mac: iOS and Android phones usually take the
shorter interval (iOS may keep its own when asked for a fixed 15 ms).

## 23.4 A complete peripheral

[`examples/23-ble-status`](examples/23-ble-status/main.gg) serves the
battery level (a value a phone reads and subscribes to) and a command
characteristic, answered on a third:

```go
	pct := 87
	ble.SetValue(p, level, bytes.FromSlice([]byte{byte(pct)}))
	println("advertising as gogo-demo")
	for {
		post, ok := actor.Receive().(port.Post)
		if !ok {
			continue // the port.Done of a SendNotify
		}
		ev, _ := ble.Event(post)
		switch e := ev.(type) {
		case ble.Connected:
			println("connected:", e.Peer, "mtu", e.MTU)
		case ble.MTUChanged:
			println("mtu", e.MTU)
		case ble.Subscribed:
			println("subscribed", e.Char, e.On)
		case ble.Disconnected:
			println("disconnected, reason", e.Reason)
		case ble.Written:
			cmd := e.Data.String()
			println("command:", cmd)
			switch cmd {
			case "ping":
				ble.SendNotify(p, reply, bytes.FromString("pong"))
			case "drain":
				pct -= 10
				v := bytes.FromSlice([]byte{byte(pct)})
				ble.SetValue(p, level, v)   // what a read returns from now on
				ble.SendNotify(p, level, v) // and a push to a subscribed phone
			case "bye":
				return // the port closes with its owner: the phone is disconnected
			}
		}
	}
```

With a phone that connects, reads the level, subscribes to both
notifying characteristics and writes `ping`, `drain` and `bye`, it prints:

```text
advertising as gogo-demo
connected: 4c:2a:91:07:5e:d3 mtu 23
mtu 185
subscribed 0 true
subscribed 2 true
command: ping
command: drain
command: bye
```

The two reads of the level (87, then 77 after `drain`) do not appear: the
driver answered them.

## 23.5 With a phone: nRF Connect

Any generic BLE app works; Nordic's free **nRF Connect for Mobile**
(Android and iOS) shows everything the port does. Flash the example and
open the app:

```
$ gogo build docs/book/examples/23-ble-status -o status.ggb
$ gogo flash status.ggb
$ gogo monitor
advertising as gogo-demo
```

1. **Scan.** On the Scanner tab, `gogo-demo` appears (filter by name if the
   room is busy). The board's address is the radio's own and stays the same.
2. **Connect.** Tap *Connect*. The monitor prints `connected: ...` and,
   a moment later, `mtu ...` (the app asks for a larger MTU).
3. **Read.** Under *Battery Service*, the *Battery Level* characteristic
   has a read arrow (the down arrow): tap it, and the value shows as
   `87%`. The program printed nothing: reads never reach it.
4. **Subscribe.** Tap the triple-arrow icon of *Battery Level* and of the
   third characteristic (`6E400003-...`, under the *Nordic UART Service*
   if the app names it). The monitor prints `subscribed 0 true` and
   `subscribed 2 true`.
5. **Write.** On the write characteristic (`6E400002-...`) tap the up
   arrow, choose *UTF-8*, type `ping` and send. The monitor prints
   `command: ping` and the app shows `pong` as the third characteristic's
   value. Send `drain`: the battery level drops to `77%` by itself (a
   notification).
6. **Disconnect.** Tap *Disconnect*: the monitor prints
   `disconnected, reason 19`, and `gogo-demo` shows up in the scanner
   again. Send `bye` instead and the program ends: the app sees the
   connection close.

On iOS, the system caches what a device offers by its address: after you
change a program's services, turn Bluetooth off and on (or forget the
device) before connecting again, or the app may show the old ones.

## 23.6 On the host

`gogovm` has the `ble` port too, so programs that use it run and are
tested without a radio. The radio is a Unix-domain socket that exists
while the port is open, at `$GOGO_BLE` (default `$TMPDIR/gogo-ble.sock`);
a client that connects to it plays the phone with a small line protocol,
documented at the top of
[`platform/unix/ble_unix.c`](../../platform/unix/ble_unix.c):

```
$ GOGO_BLE=/tmp/demo.sock gogo run docs/book/examples/23-ble-status &
advertising as gogo-demo
$ nc -U /tmp/demo.sock
adv gogo-demo
svc 180f
chr 0 2a19 9
...
end
connect 11:22:33:44:55:66 23
connected: 11:22:33:44:55:66 mtu 23
read 0
value 0 57
```

Each example in this chapter has a `phone` script next to it, which
`tests/ble` plays over the socket (it runs in `make test`); the tests there
are the place to copy from for a program of your own. On a board, the
same checks with a computer as the phone are `tools/ble_central.py`
(Python and `bleak`), which drives `tests/ble/progs/echo` through scan,
connect, read, write, subscribe, notifications, a throughput figure and
disconnect.

## 23.7 Memory, Wi-Fi and the radio

- **Memory.** BLE is in the default firmware of both boards. The firmware
  is about 136 KB larger with it (170 KB with the Wi-Fi setup from a
  phone, section 2.1), and the Bluetooth controller keeps
  about 22 KB of internal RAM even while no port is open. An open port
  costs about 65 KB more of internal RAM (the NimBLE
  host and the controller's connection state, freed when the port closes).
  `ble.Open` checks first: with less than 80 KB of internal RAM free (or
  no free block of 16 KB) it fails with `errors.NoMemory` and starts
  nothing, rather than letting the Bluetooth stack run out halfway (which
  would crash the board). On the Cardputer (no PSRAM) open the port before
  a TLS connection, or close the connection first.
  The driver's own state is fixed (about 3 KB). A firmware for a program
  that never uses Bluetooth can leave it out: `gogo firmware -without ble`
  (chapter 12).
- **Wi-Fi.** Bluetooth and Wi-Fi share one 2.4 GHz radio and antenna; the
  ESP32-S3 switches between them in time slices (coexistence). Both work at
  once, Wi-Fi and TLS included, but each is slower while the other is busy,
  and a phone may see the first connection attempt fail while Wi-Fi is
  joining a network.
- **Range.** About 10 m indoors, through a wall or two, with the boards'
  small antennas; a body between the board and the phone halves it.
- **Security.** The port does no pairing or encryption: anyone nearby can
  connect, read what the program has set, and write. Keep secrets out of
  readable values, and check what a write asks for as you would check a
  network request. (The firmware's own Wi-Fi setup from a phone, section
  2.1, is a different service, entered only with a key held at boot, and
  encrypted.)

## 23.8 What is not there

- **The central role**: scanning for other devices and connecting to
  them (a heart-rate strap, another board). The port is a peripheral
  only; a central can come later if programs need it.
- **More than one connection**, indications (notifications the phone
  acknowledges), descriptors of your own (a characteristic's user
  description), pairing and bonding.
- **Long values in one notification**: split them (23.3).
