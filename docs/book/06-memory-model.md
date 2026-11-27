# 6. Memory Model

A microcontroller has little memory: the ESP32-S3 boards give gogo about
160 KB for all actors together. How that memory is divided decides whether
one misbehaving actor can bring down the rest. gogo's answer is simple to
state: **every actor has its own memory with its own limit, and nothing is
ever shared that could be changed.**

## 6.1 One heap per actor

Each actor owns one block of memory holding both its heap (slices,
strings, structs, closures) and its stack. When the block fills up, the VM
collects garbage *for that actor only*, by copying its live data to a new
block, which can also grow or shrink the block. Other actors are not
stopped or touched. An actor waiting in `Receive` with little live data is
shrunk back to a small block automatically.

Because of this, a gogo actor is cheap. An idle actor (blocked in
`Receive`, nothing in its mailbox) costs about 150 bytes in total: a
48-byte control block, a small memory block for its stack frame, and a
4-byte slot in the actor table. Links, monitors and names are kept outside
the control block and cost nothing until used.

```go
// Actors are cheap: an idle actor waiting in Receive costs about 150
// bytes. Spawn a thousand, measure one, then stop them all.
package main

import "gogo/actor"

type Quit struct{}

func idle() {
	for {
		switch actor.Receive().(type) {
		case Quit:
			return
		}
	}
}

func main() {
	var pids []actor.PID
	for i := 0; i < 1000; i++ {
		pids = append(pids, actor.Spawn(idle))
	}
	actor.Sleep(10) // let them all reach Receive
	m := actor.Info(pids[500]).Memory
	println("1000 idle actors; one uses", m, "bytes, under 256:", m <= 256)
	for _, p := range pids {
		actor.Send(p, Quit{})
	}
	println("all asked to quit")
}
```

```text
1000 idle actors; one uses 152 bytes, under 256: true
all asked to quit
```

`actor.Info(p).Memory` reports everything an actor is charged for: its
control block, its memory block, its link and monitor records, and the
messages waiting in its mailbox.

## 6.2 Memory limits

Every actor has a **memory limit**: its heap, its stack and its mailbox
together. The limits are the VM's parameters, with a default per platform:

| Limit | ESP32-S3 boards | host VM (`gogo run`, gogovm, libgogo) | Config key | gogovm flag |
|---|---|---|---|---|
| memory per actor (default) | 16 KB, at most 256 KB | 1 MB, at most 1 GB | `vm.actor_memory` | `--actor-memory` |
| memory for all actors | 160 KB (the arena; its ceiling) | no limit | `vm.memory` | `--memory` |
| live actors | 1024 | 16384, at most 65520 | `vm.max_actors` | `--max-actors` |
| open ports | 16 | 1024, at most 2048 | `vm.max_ports` | `--max-ports` |
| node links | 8 | 512, at most 2048 | `node.max_links` | `--max-links` |
| loaded module code | 64 KB | 16 MB | `code.mem` | |
| off-heap `Bytes`, VM-wide / per actor | 64 / 16, at most 1024 | 4096 / 256, at most 65535 | `vm.handles` | `--bytes-handles` |

**Raising a limit.** The VM reads them once, when it starts, in this
order: a gogovm flag (or, in an embedding host, a field of its
`gogo_config`, section 18.3) beats the config key, and the key beats the
platform default. On a board, set the key and restart it:

```sh
gogo config set vm.actor_memory 24K     # every actor may now use 24 KB
gogo device info                        # limits    actor_memory=24K memory=160K ...
```

On your computer, a flag for one run or the key for every run:

```sh
gogo run --actor-memory 4M myprog       # or: gogovm --actor-memory 4M myprog.ggb
gogo config -local set vm.actor_memory 4M
gogovm --info                           # actor_memory=4M memory=unlimited ...
```

Sizes take a `K`, `M` or `G` suffix. A value past the platform's ceiling is
cut to it, with one line on the console (`gogo: vm.actor_memory 2M is over
this VM's 262140; using 262140` on a board, where an actor holds at most
65535 words), and one that is not a number is ignored with a line; `gogo
config set` refuses both before writing. A board's ceilings come from its
RAM: raising `vm.actor_memory` lets one actor take more of the 160 KB, it
does not make more. Chapter 12 lists the flags, chapter 11.3 the keys.

`SpawnOpt` sets a different limit for one actor, up to the ceiling, and
`actor.Limit(p)` reports an actor's limit, so a program can check its
budget instead of guessing (image format 15.8):

```go
func SpawnOpt(f func(), opts Options) PID
type Options struct{ MaxMemory int } // bytes; 0 = the platform default
```

An actor that needs more than its limit, even after a garbage collection,
exits with `OutOfMemory`. Nobody else is affected:

```go
// Every actor has a memory limit (the platform default, or Options.
// MaxMemory with SpawnOpt). Going over it ends that actor with
// OutOfMemory; nobody else is affected. actor.Info reports usage.
package main

import "gogo/actor"

func grow() {
	var xs []int
	for {
		xs = append(xs, len(xs))
		if len(xs)%1000 == 0 {
			println("  holding", len(xs), "ints, memory", actor.Info(actor.Self()).Memory >= len(xs)*4)
		}
	}
}

func main() {
	me := actor.Self()
	println("main uses", actor.Info(me).Memory, "bytes")
	p := actor.SpawnOpt(func() { grow() }, actor.Options{MaxMemory: 16384})
	actor.Monitor(p)
	switch m := actor.Receive().(type) {
	case actor.DownMsg:
		println("grower exited, OutOfMemory:", m.Reason.Code == actor.OutOfMemory)
	}
	println("main still runs")
}
```

```text
main uses 168 bytes
  holding 1000 ints, memory true
  holding 2000 ints, memory true
grower exited, OutOfMemory: true
main still runs
```

Standard error:

```text
gogo: actor <pid 1> crashed: oom in grow
```

A limit smaller than the actor's starting block is raised to fit it, and a
huge one is cut to the platform's ceiling (256 KB on a board, 1 GB on the
host).

> **Tip.** Programs are usually tested with `gogo run`, where the default
> limit is 64 times larger than on the board. An actor that works on
> your computer can run out of memory on the board. Give memory-hungry
> actors an explicit `SpawnOpt` limit during development, or run with the
> board's numbers (`gogo run --actor-memory 16K --memory 160K prog`), so the
> host behaves like the board.

If the whole VM runs out of memory (all actors together are using the
160 KB, or `vm.memory`), the actor that is trying to allocate is ended with `OutOfMemory`.
The VM keeps a small emergency reserve so that it can still finish that
exit and deliver the notifications.

## 6.3 Messages are copied

`actor.Send` copies the message into the receiver's memory: the struct, the
slices it points to, the strings, the closures, everything reachable from
it. The receiver gets an independent copy. Inside the message, sharing is
kept: if two fields refer to the same slice, the copy's two fields refer to
the same copied slice.

```go
// Send copies the whole message, including slices and nested structs. The
// receiver's copy keeps the sharing inside the message (two views of one
// slice stay one slice), but nothing is shared between the two actors.
package main

import "gogo/actor"

type Batch struct {
	Name  string
	All   []int
	First []int // a view into All
}

func main() {
	me := actor.Self()
	nums := []int{1, 2, 3, 4}
	b := Batch{Name: "b1", All: nums, First: nums[:2]}

	w := actor.Spawn(func() {
		switch m := actor.Receive().(type) {
		case Batch:
			m.First[0] = 99 // First and All share one array in the copy too
			println("receiver:", m.All[0], m.First[0])
			actor.Send(me, true)
		}
	})
	actor.Send(w, b)
	nums[1] = -2 // after the send: the receiver never sees this
	actor.Receive()
	println("sender:", nums[0], nums[1], b.First[1])
}
```

```text
receiver: 99 99
sender: 1 -2 -2
```

The receiver's write to `First[0]` showed up in its own `All[0]` (shared
inside the copy), and the sender's later write to `nums[1]` did not reach
the receiver at all.

> **Note.** This is the one place where gogo and Go mean different things
> by the same code. In Go, sending a slice over a channel shares the
> underlying array. In gogo the receiver gets a copy, always. The price is
> the copying; the gain is that no actor can ever see another one's data
> change under it.

Some values are not copied because they cannot change: PIDs, Refs, ports,
string constants from your program (they stay in flash), and large frozen
`bytes.Bytes` (next section).

Closures can be sent in messages too: the function and its captured values
travel together.

## 6.4 Mailboxes count against the receiver

A message waiting in a mailbox has already been copied, so it uses memory.
That memory is charged to the **receiver**. If a receiver falls behind and
its queued messages take it over its limit, the receiver exits with
`MailboxOverflow`. The sender is never blocked or harmed:

```go
// Queued messages count against the RECEIVER's memory limit. A receiver
// that falls behind exits with MailboxOverflow; the sender is never
// slowed down or killed.
package main

import "gogo/actor"

type Sample struct{ N, Value int }

func main() {
	slow := actor.SpawnOpt(func() {
		actor.Sleep(1000) // never reads in time
	}, actor.Options{MaxMemory: 4096})
	actor.Monitor(slow)
	for i := 0; i < 1000; i++ {
		actor.Send(slow, Sample{i, i * i})
	}
	println("sender sent 1000 messages")
	switch m := actor.Receive().(type) {
	case actor.DownMsg:
		println("receiver exited, MailboxOverflow:", m.Reason.Code == actor.MailboxOverflow)
	}
}
```

```text
sender sent 1000 messages
receiver exited, MailboxOverflow: true
```

Standard error:

```text
gogo: actor <pid 1> crashed: mailbox_overflow
```

This rule protects the system from the most common actor bug, a fast
producer feeding a slow consumer. The consumer fails, visibly, and its
supervisor restarts it with an empty mailbox; the producer carries on.
Messages sent to it after it exited are dropped.

Port events differ. GPIO edges are ordinary messages: an owner that never
reads them overflows like any receiver. Keyboard, battery and UDP events
that do not fit the owner's limit are dropped instead (the owner is not
killed), and TCP data waits until the owner has room. See chapters 7 to 10.

## 6.5 Bytes and Buffer: big data without copies

Copying is fine for small messages. For a 4 KB file chunk or a screen
frame it is not. `gogo/bytes` has two types for byte data:

- **`bytes.Buffer`** is a growable, writable buffer that belongs to one
  actor. Make one with `bytes.NewBuffer(capacity)` and append with
  `WriteString`, `Write`, `WriteByte` and `WriteBytes`. A `Buffer` is a
  reference type: `b2 := b` refers to the same buffer.
- **`bytes.Bytes`** is an immutable sequence of bytes. `buf.Freeze()`
  turns the buffer's contents into a `Bytes` and empties the buffer for
  reuse. `bytes.FromString` and `bytes.FromSlice` copy into one.

A `Bytes` larger than 64 bytes made by `Freeze` lives outside the actor's
heap, with a reference count. Sending it passes a reference: no copy, no
matter the size. Since nobody can change it, sharing is safe.

```go
// bytes.Buffer builds data; Freeze turns it into an immutable bytes.Bytes.
// A frozen Bytes over 64 bytes lives outside the actor heap and is shared,
// not copied, when sent.
package main

import (
	"gogo/actor"
	"gogo/bytes"
)

type Frame struct {
	Seq  int
	Data bytes.Bytes
}

func main() {
	buf := bytes.NewBuffer(256)
	for i := 0; i < 200; i++ {
		buf.WriteByte(byte('a' + i%26))
	}
	buf.WriteString("!")
	frame := buf.Freeze() // buf is empty again and can be reused
	println("frozen", frame.Len(), "bytes; buffer now", buf.Len())
	println("first", frame.Slice(0, 5).String(), "last", frame.At(frame.Len()-1))

	me := actor.Self()
	sink := actor.Spawn(func() {
		switch m := actor.Receive().(type) {
		case Frame:
			// The receiver holds the same 201 bytes, charged to it too.
			println("sink got frame", m.Seq, m.Data.Len(), m.Data.Slice(26, 29).String())
			actor.Send(me, actor.Info(actor.Self()).Memory > 201)
		}
	})
	actor.Send(sink, Frame{1, frame})
	println("sink charged for it:", actor.Receive())

	// Small helpers: from strings and slices, compare, copy out.
	a := bytes.FromString("ping")
	b := bytes.FromSlice([]byte{'p', 'i', 'n', 'g'})
	println(a.Equal(b), a.String())
	out := make([]byte, 2)
	println(a.CopyTo(out), string(out))
}
```

```text
frozen 201 bytes; buffer now 0
first abcde last 33
sink got frame 1 201 abc
sink charged for it: true
true ping
2 pi
```

Reading a `Bytes`: `Len()`, `At(i)`, `Slice(lo, hi)`, `String()` (a copy),
`Equal(other)` (`==` is not allowed), `CopyTo(p []byte)`. A `Slice` of a
large `Bytes` shares it (and keeps all of it alive). Out-of-range indexes
crash the actor with `badcode`.

The rules that keep this honest:

- Every actor that holds a shared `Bytes` is **charged its full size**,
  including a receiver that has it waiting in its mailbox. Sharing saves
  the copy, not the accounting, so a limit still means what it says.
- An actor can hold at most **16** off-heap `Bytes` at a time, and the
  whole VM at most **64**. Going over ends the actor with `OutOfMemory`.
- A `Bytes` is freed when no actor can reach it any more (at the holder's
  next garbage collection or exit). A local variable keeps its `Bytes`
  alive until the end of its block, so drop a big one early by using it in
  an inner block or reassigning the variable.
- The zero `Buffer` is not usable. The compiler rejects `var b
  bytes.Buffer` and `bytes.Buffer{}` (`zero value of bytes.Buffer not
  supported: use bytes.NewBuffer`); a zero `Buffer` it cannot see (a field
  of a zero struct) crashes with `badcode` when used.
- A `Buffer` sent in a message is copied like other data.

The file, network and HTTP APIs all take and return `bytes.Bytes`.

## 6.6 Constants live in flash

The program image stays in flash and runs in place. String constants and
other constant data are read from flash and never copied into RAM, and
sending a constant string sends a reference. Package-level variables live
there too (chapter 3): they are read-only, set when the program is built,
so data that is the same for every actor costs no RAM, and data that
changes belongs to an actor. A slice or map of one stays read-only
wherever it goes, sent in a message included.

## 6.7 Summary

| Question | Answer |
|---|---|
| Who owns a value? | The actor whose heap it is in. Nobody else can reach it. |
| What does `Send` do? | Copies the message into the receiver's mailbox (except PIDs, Refs, constants and large frozen `Bytes`). |
| Who pays for a queued message? | The receiver. |
| What happens over the limit? | That actor exits: `OutOfMemory` or `MailboxOverflow`. |
| How do I move big data? | Build it in a `Buffer`, `Freeze` it, send the `Bytes`. |
| How do I free memory? | Let values go out of scope; each actor collects its own garbage. When an actor exits, all of its memory goes at once. |
