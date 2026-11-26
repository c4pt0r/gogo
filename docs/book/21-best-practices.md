# 21. Best Practices

The earlier chapters say what gogo does. This one says how to use it well.
Each section states a problem, the rule that avoids it, and why the rule
follows from how gogo works. It assumes chapters 4 to 7, and refers to the
chapters on streams (15), audio (16) and HTTPS (17) where they matter.

The short version: every actor is a small machine with its own memory
budget; messages are copies, so keep them small and move bulk data as
`bytes.Bytes`; never let a producer outrun a consumer; and keep the
per-byte work in C drivers, where the data plane belongs.

## 21.1 Treat each actor as a small machine

**Problem.** A program passes under `gogo run` and fails on the board with
`oom`.

**Rule.** Give every actor that does real work an explicit memory limit
with `actor.SpawnOpt`, sized from a measurement, and test with board
limits.

**Why.** Every actor has its own memory limit (section 6.2): its heap, its
stack and the messages waiting in its mailbox count against it. The
default is **16 KB on a board** and **256 KB on the host**, sixteen times
more. On top of that, all actors on an ESP32-S3 board share about
**160 KB**, so ten actors at their full default already take it all. An
idle actor, by contrast, costs about **150 bytes** (section 6.1): many
small actors are cheap, a few big ones are not.

```go
func SpawnOpt(f func(), opts Options) PID
type Options struct{ MaxMemory int } // bytes; 0 = the platform default
func Info(p PID) ProcInfo           // ProcInfo{Memory int}: bytes charged now
```

This program runs the same job (a table of 5000 ints) three times, with
three limits:

```go
// table builds a lookup table of n entries and reports its own memory.
func table(boss actor.PID, n int) {
	t := make([]int, 0, n)
	for i := 0; i < n; i++ {
		t = append(t, i*i)
	}
	actor.Send(boss, Result{actor.Info(actor.Self()).Memory})
}

// run starts table with the given limit (0: the platform default) and
// says how it ended.
func run(label string, limit int) {
	me := actor.Self()
	p := actor.SpawnOpt(func() { table(me, 5000) }, actor.Options{MaxMemory: limit})
	actor.Monitor(p)
	for {
		switch m := actor.Receive().(type) {
		case Result:
			println(label, "ok, uses more than 16 KB:", m.Peak > 16384)
		case actor.DownMsg:
			if m.Reason.Code == actor.OutOfMemory {
				println(label, "OutOfMemory")
			}
			return
		}
	}
}

func main() {
	run("default limit:", 0)         // 256 KB under gogo run, 16 KB on a board
	run("board-sized limit:", 16384) // what a board gives by default
	run("sized for the job:", 32768) // measured, with room to spare
}
```

Under `gogo run`:

```text
default limit: ok, uses more than 16 KB: true
board-sized limit: OutOfMemory
sized for the job: ok, uses more than 16 KB: true
```

Standard error:

```text
gogo: actor <pid 1> crashed: oom in table
```

The first line is the trap: with the default limit the job passes on the
host, and on a board it would fail like the second. `make test-go` also
builds `build/gogovm_mcu`, a host VM with the board's limits (16 KB per
actor, 160 KB in all, 1024 actors); `GOGO_VM=build/gogovm_mcu gogo run .`
runs a program with them, and there the first line says `OutOfMemory`
too. The third run, with a limit chosen from `actor.Info`, works on both.

Habits that follow:

- Measure with `actor.Info(p).Memory` at the actor's busiest moment (a full
  mailbox, the largest message it handles), then set `MaxMemory` with a
  margin.
- Add up the limits of the actors that can be busy at the same time. If
  the sum is far above 160 KB, the design does not fit the board, whatever
  the host says.
- Prefer several small actors to one big one: a big actor costs memory and,
  as section 21.8 shows, time for everyone else.

## 21.2 Keep messages small; move bulk data as `bytes.Bytes`

**Problem.** Passing large values between actors is slow and uses memory
twice.

**Rule.** Messages are small structs. Bulk data (a file chunk, a sound
frame, a screen) travels as a `bytes.Bytes`, built in a `bytes.Buffer` and
frozen, and is released as soon as it is not needed.

**Why.** `actor.Send` deep-copies the message into the receiver's memory
(section 6.3). Copying is proportional to the size, it cannot be
preempted (section 21.8), and for a while both actors hold the data. A
frozen `Bytes` larger than 64 bytes is different: it lives outside the
actor heaps with a reference count, and sending it passes a reference
(section 6.5). It costs the copy once, at `Freeze`.

What a shared `Bytes` does **not** save is accounting:

- **Every holder is charged its full size**, including a receiver that
  still has it waiting in its mailbox. Two actors holding the same 4 KB
  frame are charged 4 KB each.
- **At most 16 off-heap `Bytes` per actor and 64 in the whole VM.** One
  more ends the actor with `OutOfMemory`.
- **A local variable keeps its `Bytes` alive to the end of its block**, and
  a `Slice` of a large `Bytes` keeps all of it alive. An actor that keeps
  the last frame in a variable while it waits in `Receive` holds that frame,
  its charge and its handle for as long as it waits. A `Bytes` nobody
  reaches any more is freed at its holder's next garbage collection.

```go
// holder sleeps first, so a frame sent to it waits in its mailbox.
func holder() {
	actor.Sleep(50)
	for {
		actor.Receive()
	}
}

// hoarder keeps every frame it makes, until it runs out of handles.
func hoarder(boss actor.PID) {
	var kept []bytes.Bytes
	for {
		kept = append(kept, frame(1000))
		actor.Send(boss, len(kept))
	}
}

func main() {
	f := frame(4000)
	a := actor.Spawn(holder)
	b := actor.Spawn(holder)
	actor.Sleep(10) // both are asleep now
	before := actor.Info(a).Memory
	actor.Send(a, Frame{1, f})
	actor.Send(b, Frame{1, f})
	println("queued frame charged to a:", actor.Info(a).Memory-before >= 4000)
	println("and to b as well:", actor.Info(b).Memory-before >= 4000)
```

Then `main` starts a `hoarder` under a monitor and waits for it to die.
(`frame(n)` fills a `Buffer` with `n` bytes and freezes it; the whole
program is the book example `21-bytes-cost`.)

```text
queued frame charged to a: true
and to b as well: true
hoarder held 16 frames, then: true
```

Not every `Bytes` is off-heap. The off-heap ones are mainly the results
of `Buffer.Freeze`, `fs.File.Read` and `fs.ReadFile` over 64 bytes, and
large slices of those. The `Data` of a stream port's `port.Chunk` is built
inside the message and lands on the receiver's heap, like a small
`Bytes`: **forwarding a `Chunk`'s data to another actor copies it**. For one hop that is the same copy `Freeze` would make;
when one frame goes to several actors, or through several stages, copy it
into a `Buffer` and `Freeze` it once, then pass the frozen one.

## 21.3 Backpressure

**Problem.** A producer sends faster than its consumer handles messages.

**Rule.** Never let a producer run ahead of its consumer without a bound.
Use `Call` or acknowledgements to keep a fixed number of messages in
flight, let the consumer pull, or reduce the data at the source.

**Why.** `Send` never blocks (section 4.1), and queued messages count
against the **receiver's** limit (section 6.4). A consumer that falls
behind is not slowed down: its mailbox grows until it exits with
`MailboxOverflow`, and what is sent to it afterwards is dropped. Section
6.4 shows exactly that: [1000 `Sample`s to a 4 KB consumer that never
reads](06-memory-model.md#64-mailboxes-count-against-the-receiver). This
protects the rest of the system, but the data is lost and the consumer
restarts empty.

The patterns:

- **Acknowledgements (a window).** The producer keeps at most N messages
  unacknowledged; the consumer answers each (or each batch) with an ack.
  The consumer's mailbox then never holds more than N, so its memory need
  is known.
- **`Call`.** A window of one: the producer waits for each answer. Simple,
  and slower, since nothing overlaps.
- **Pull.** The consumer asks for the next item when it is ready ("give me
  more"), and the producer keeps what is not asked for, or drops it.
- **Reduce at the source.** Average, downsample, batch or drop before
  sending: one message with the mean of 100 samples instead of 100
  messages. Ports do this themselves: the IMU sends one `Chunk` per
  `BatchMs`, and stream ports drop and count rather than overflow (section
  21.9).

Here is section 6.4's producer and consumer again, with a window of 4:

```go
const Window = 4

type Sample struct {
	From     actor.PID
	N, Value int
}

type Ack struct{}

type Total struct{ N, Sum int }

func consumer(boss actor.PID) {
	n, sum := 0, 0
	for n < 1000 {
		switch m := actor.Receive().(type) {
		case Sample:
			if n%100 == 0 {
				actor.Sleep(1) // slow work
			}
			n++
			sum += m.Value
			actor.Send(m.From, Ack{})
		}
	}
	actor.Send(boss, Total{n, sum})
}

func main() {
	me := actor.Self()
	c := actor.SpawnOpt(func() { consumer(me) }, actor.Options{MaxMemory: 4096})
	actor.Monitor(c)
	inFlight := 0
	for i := 0; i < 1000; i++ {
		for inFlight == Window { // wait for room
			switch m := actor.Receive().(type) {
			case Ack:
				inFlight--
			case actor.DownMsg:
				println("consumer died, reason", m.Reason.Code)
				return
			}
		}
		actor.Send(c, Sample{me, i, i % 10})
		inFlight++
	}
```

```text
consumer got 1000 samples, sum 4500
```

The consumer has the same 4 KB limit as in section 6.4 and gets all 1000
samples. The producer runs at the consumer's pace. The window is also the
memory rule: a consumer's limit must hold its heap plus `Window` messages.

## 21.4 Shared state lives in an actor

**Problem.** Several actors need the same changing data: settings, a
count, a cache.

**Rule.** One actor owns each piece of state and is the only one that
changes it. Others ask it (`Call`), tell it (`Send`), or subscribe to
snapshots it pushes. When one owner becomes a bottleneck, split the state
by key over several owners.

**Why.** There is nothing else. Package-level variables are read-only
(section 3.2, they live in flash), and actors share no memory. The owner's
mailbox serializes every change, so there are no locks and no races: two
updates are simply two messages, handled one after the other. A snapshot
sent to a reader is a copy, so the reader cannot change the owner's data,
and it is up to date only until the next change.

```go
func owner(shard int) {
	var counts []Count
	var subs []actor.PID
	for {
		switch m := actor.Receive().(type) {
		case Incr:
			counts = incr(counts, m.Key)
			for _, s := range subs {
				actor.Send(s, Snapshot{shard, counts}) // a copy: readers cannot change it
			}
		case Subscribe:
			subs = append(subs, m.To)
		case actor.Request:
			switch g := m.Body.(type) {
			case Get:
				actor.Reply(m, find(counts, g.Key))
			}
		}
	}
}
```

```go
// shardFor picks the owner of a key: the same key always goes to the same
// shard.
func shardFor(key string, shards []actor.PID) actor.PID {
	h := 0
	for i := 0; i < len(key); i++ {
		h = (h*31 + int(key[i])) % 1024
	}
	return shards[h%len(shards)]
}
```

```go
func main() {
	shards := []actor.PID{actor.Spawn(func() { owner(0) }), actor.Spawn(func() { owner(1) })}
	d := actor.Spawn(display)
	for _, s := range shards {
		actor.Send(s, Subscribe{d})
	}
	for _, k := range []string{"door", "fan", "door", "window", "door"} {
		actor.Send(shardFor(k, shards), Incr{k})
	}
	n, ok := get("door", shards)
	println("door:", n, ok)
}
```

The `display` actor prints each snapshot it gets:

```text
shard 0: door=1
shard 0: door=2
shard 0: door=2 window=1
shard 0: door=3 window=1
shard 1: fan=1
door: 3 true
```

Push snapshots when readers need the state often and it changes rarely
(a reader then answers from its own copy, without a round trip); answer
`Call`s when it changes often and is read rarely. Snapshots are copies, so
keep them small, or send only what changed. Sharding helps only when the
keys are independent: an operation that needs two shards at once is a
sign the split is wrong. Section 10.5's web server keeps its hit counter
the same way: handlers live for one request, the count lives in an actor.

## 21.5 Call discipline

**Problem.** An actor waits forever, or two actors wait for each other.

**Rule.** Every `Call` has a timeout, and the caller handles `ok ==
false`. Requests flow in one direction: if A calls B, B never calls A.

**Why.** `Call` waits only for its own reply and leaves every other
message in the mailbox (section 4.3). It does not notice that the server
died: it waits out the whole timeout (monitor the server to know sooner).
A reply that comes after the timeout stays in the caller's mailbox,
counted against its limit, until the next `Receive` or `Call` scans past
it. So treat `ok == false` as "no answer, maybe dead", and keep timeouts
short enough that a dead server costs little. A server on another board
adds lost nodes and lost messages; sections
[20.5](20-distributed.md#205-when-a-node-goes-away) and
[20.6](20-distributed.md#206-delivery) say when such a Call fails at once
and when it waits out its timeout.

A cycle is worse. If A calls B and B, while handling that request, calls
A, then A is waiting in `Call` for B's reply, and B's request sits in A's
mailbox behind it. Neither can go on until a timeout fires:

```go
func sensor(boss actor.PID) {
	for {
		switch r := actor.Receive().(type) {
		case actor.Request:
			switch b := r.Body.(type) {
			case Read:
				// Calls up the hierarchy: a cycle with the controller.
				scale, ok := actor.Call(boss, GetScale{}, 200)
				if !ok {
					println("  sensor: GetScale timed out")
					scale = 0
				}
				actor.Reply(r, 42*scale.(int))
			case ReadWith:
				actor.Reply(r, 42*b.Scale)
			}
		}
	}
}
```

The controller calls the sensor with a `Read` (in the first run) or a
`ReadWith{Scale: 10}` (in the second); the program measures each:

```text
  sensor: GetScale timed out
cycle: 0 true slow: true
fixed: 420 true slow: false
```

With the cycle the answer is wrong (0, not 420) and late (200 ms), and the
controller still holds the sensor's `GetScale` in its mailbox; when it
answers it, the reply goes to a `Call` that has given up. The fix is a
**one-directional request hierarchy**: the controller sends what the
sensor needs with the request (`ReadWith`), and the sensor never calls
up. When a lower actor must tell a higher one something, it uses `Send`
(an event), never `Call`, and the higher one handles it like any other
message. If the higher one must answer, it answers with another `Send`,
and the lower one waits for that reply message in its own receive loop,
where it can also handle everything else that arrives.

## 21.6 Be ready to restart

**Problem.** After a supervisor restarts an actor, its data is gone.

**Rule.** Keep what must survive a crash in storage (`gogo/kv` for small
values, files for larger ones) and reload it when the actor starts. Make
everything else cheap to rebuild.

**Why.** A restarted actor is a new actor: a new PID, an empty heap, an
empty mailbox (section 5.6). Its ports were closed with the old one, its
names freed, its links gone. That is the point of "let it crash": the new
instance starts from a known state. Whatever the program cannot afford to
lose must be outside the actor when it crashes.

```go
// load reads the stored count: 0 if there is none yet.
func load() int {
	v, e := kv.Get("count")
	if e != fs.OK {
		return 0 // fs.NotFound on the first start
	}
	n, err := strconv.Atoi(v.String())
	if err != nil {
		return 0
	}
	return n
}

func counter() {
	n := load()
	println("counter starts at", n)
	actor.Register("counter", actor.Self())
	for {
		switch r := actor.Receive().(type) {
		case actor.Request:
			switch r.Body.(type) {
			case Incr:
				n++
				kv.Put("count", strconv.Itoa(n)) // persist before answering
				actor.Reply(r, n)
			case Get:
				actor.Reply(r, n)
			case Crash:
				zero := 0
				println(n / zero)
			}
		}
	}
}
```

`main` supervises `counter`, counts to 3, makes it crash, and counts once
more; its helper finds the counter by name each time, so it reaches the
restarted instance:

```text
counter starts at 0
counter starts at 3
after the crash: 4 true
```

Standard error:

```text
gogo: actor <pid 2> crashed: divzero in counter
```

The snake game (`examples/snake` in the gogo tree) keeps its high score the same way: it
reads `snake.hi` with `kv.Get` at start and writes it with `kv.Put` when a
game beats it. Habits:

- **Persist before acknowledging.** The counter writes before it replies,
  so a client that got an answer knows the value is stored.
- **Treat only `fs.NotFound` as "no value yet"** (section 9.5). Another
  error is a real failure; replacing the value with a default would destroy
  data you merely failed to read.
- **Keep startup cheap.** An actor that takes seconds to rebuild its state
  makes every crash cost seconds. Store compact state, not logs to replay.
- **Write rarely.** `/flash` wears and its writes pause every actor
  (section 21.8). Save on change, not on a timer, and not for every sample.
- **Re-register names and re-open ports at start**, so clients that look
  services up by name find the new instance. Clients should not keep a
  PID across a restart.

## 21.7 Receive takes the oldest message

**Problem.** An actor gets a message it did not expect at that moment.

**Rule.** Handle every message type the actor can receive, in every
state. Tag requests with an ID and drop answers to requests you gave up
on.

**Why.** `Receive` always returns the oldest message in the mailbox,
whatever it is. There is no selective receive: an actor cannot wait for
one kind of message and leave the others queued. The only exception is
`Call`'s wait for its own reply, which is internal to `Call` (section
4.3). And messages from one sender arrive in order, but messages from
different senders interleave in any order.

So a state machine must cope with messages that arrive "early" (a `Data`
before the `Start` it belongs to, from another sender) and "late" (the
answer to a request that already timed out, an event from a port closed a
moment ago). For each state decide what to do with each type: handle it,
keep it for later, answer "not now", or drop it on purpose.

```go
// ask sends query id and waits at most timeoutMs for its answer.
func ask(s actor.PID, id, delayMs, timeoutMs int) (int, bool) {
	actor.Send(s, Query{id, delayMs, actor.Self()})
	deadline := actor.Now() + int64(timeoutMs)
	for {
		left := int(deadline - actor.Now())
		if left <= 0 {
			return 0, false
		}
		m, ok := actor.ReceiveTimeout(left)
		if !ok {
			return 0, false
		}
		switch a := m.(type) {
		case Answer:
			if a.ID == id {
				return a.Value, true
			}
			println("  dropped the late answer to query", a.ID)
		default:
			println("  dropped a message of an unexpected type")
		}
	}
}

func main() {
	s := actor.Spawn(server)
	v, ok := ask(s, 1, 100, 50) // the server needs 100 ms: give up after 50
	println("query 1:", v, ok)
	actor.Send(actor.Self(), "noise")
	v, ok = ask(s, 2, 0, 500)
	println("query 2:", v, ok)
}
```

```text
query 1: 0 false
  dropped a message of an unexpected type
  dropped the late answer to query 1
query 2: 200 true
```

Without the ID check, `ask(s, 2, ...)` would have returned the answer to
query 1. Note that this loop drops whatever else arrives while it waits;
an actor that must not lose those messages keeps them in a slice and
handles them after the wait, or, better, does not wait at all: it sends
the query, returns to its main loop, and handles the answer there as one
more message type. A `default` case in the main loop makes unexpected
messages visible instead of silently dropped.

## 21.8 What belongs in C and what in gogo

**Problem.** One busy actor makes everything else late.

**Rule.** Loops over thousands of elements per second belong in C, in a
driver or port. Decisions made a few to a few hundred times per second
belong in actors.

**Why.** One VM thread runs every actor. Actors are preempted by
reductions (every call and loop iteration costs one; an actor is switched
out after 1000, section 4.1), so a long loop in gogo does not freeze the
others. But it does use the one thread, and gogo code is interpreted:
every byte touched in a loop costs many machine instructions. Some
operations are not preempted at all and stall every actor while they run
(`docs/known-issues.md`):

- copying a large message, and the mailbox scan of `Call`;
- **garbage collection.** An actor's heap is collected by copying its live
  data to a new block. A big actor with a lot of live data takes a long
  collection, and nothing else runs meanwhile;
- storage calls: tens of milliseconds on `/flash` (an erase), sometimes
  hundreds on `/sd`;
- the TLS key exchange: one step of about **200 ms** in every handshake
  (the rest of the 2.5 s handshake is sliced, chapter 17);
- the IMU's first open after boot, about 250 ms.

So the split is by rate. A driver moves samples with DMA, fills buffers
and hands the VM whole frames; an actor decides what to do with each
frame. Ports are built this way: the IMU driver gathers samples for
`BatchMs` and sends one `Chunk`, so the spirit level of section 15.1 runs
its loop over 5 samples five times a second, not 25 times a second over
one:

```go
	_, err := port.Open("imu", imu.Config{RateHz: 25, BatchMs: 200})
	if err != nil {
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
```

At 25 Hz that is fine in gogo. Filtering 16 000 audio samples a second,
compressing an image or computing an FFT is not: there is no port for
those today, and they would be C functions behind a port (chapter 22
shows how to write one). In gogo, pass
such data through without touching each byte (write it to a file, send it
on a socket) and keep the per-byte loops for small things: a peak over a
short frame, the line parser of section 21.10.

Habits that follow:

- Keep the heap of busy actors small: big state means long collections.
  Large data belongs in off-heap `Bytes`, which a collection does not copy.
- Do slow storage and HTTPS work in actors whose latency does not matter,
  and do not start an https request while something real-time (recording
  to a card) is running.
- When a gogo loop shows up as a problem, move it into a driver: that is
  what ports are for.

## 21.9 High-bandwidth data: audio, sensors, (future) cameras

Audio, fast sensors and, one day, cameras produce data continuously, at
rates where the rules above stop being style and decide whether a design
works at all.

### Two layers

Split the work in two:

- **The data plane** is the C driver or port: DMA, the codec, timing, the
  buffers the hardware fills. It runs at the hardware's rate and keeps its
  buffers outside the actors' heaps. The microphone driver keeps 11 DMA
  buffers of 30 ms on a board, enough to ride out a 300 ms stall of the VM.
- **The control plane** is actors: open and close, start and stop, what
  to do with each frame, where to send it, what to do on failure.

Actors see the data only in whole frames, a few to a few dozen times per
second.

### The stream protocol

Every port that moves bytes speaks the same four messages (section 7.6,
and the exact rules at the start of chapter 15):

- **Input** is `port.Chunk{From, Seq, TimeMs, Dropped, Data}`. `Seq`
  counts from 1, dropped chunks included, so a gap means loss; `Dropped`
  says how many were lost since the last one that arrived; `TimeMs` is when
  the data began.
- **Output** is `port.Out{Data}`, answered by one `port.Done` each. At most
  **two `Out`s are in flight**; a third is refused with `Done{Seq: -1}`.
  Send two, then one more for each `Done`, and the device never runs dry
  between them (the speaker plays the second right after the first,
  section 16.1).
- **A stream never kills its owner.** A `Chunk` that does not fit the
  owner's memory is dropped and counted in the next one's `Dropped`; a
  `Done` that does not fit waits until there is room.

That last rule shapes everything else: when a recorder falls behind, it
loses frames, visibly, and keeps running. Note that a loss is reported by
the *next* `Chunk`; frames lost just before the port closes are never
reported.

### A pipeline of actors

A recorder that captures sound and writes it to storage is a pipeline:

```
mic port ──Chunk──► capture ──Frame──► writer ──► /sd (or the network)
                       ◄────Written────
```

Each stage is its own actor with its own limit, and each can fail and be
restarted without taking the others with it: a full card stops the
writer, not the capture. The stages pass frames with the window of section
21.3, so no stage can drown the next one.

Here is such a pipeline, runnable on the host with the simulated
microphone (section 16.5; the stdin script is the one chapter 16's
recorder uses: six `mic tone 440 250` lines, each followed by `sleep
300`). The capture actor opens the mic itself, so the port's memory
charge falls on its own `MaxMemory` (section 16.1), keeps at most 2 frames
unacknowledged at the writer and at most 4 waiting, and drops the oldest
waiting frame when the writer falls further behind:

```go
func capture(w actor.PID) {
	var mic actor.PID
	switch req := actor.Receive().(type) {
	case actor.Request: // the opener's Call: open the mic here, under this actor's limit
		p, err := port.Open("mic", audio.Mic{RateHz: RateHz, FrameMs: FrameMs})
		actor.Reply(req, err == nil)
		if err != nil {
			return
		}
		mic = p
	}
	me := actor.Self()
	var queue []Frame
	var boss actor.PID
	seq, inFlight, micDropped, queueDropped := 0, 0, 0, 0
	stopping := false
	for {
		switch m := actor.Receive().(type) {
		case port.Chunk:
			micDropped += m.Dropped
			seq++
			if len(queue) == MaxQueued {
				queue = pop(queue) // the writer is behind: drop the oldest
				queueDropped++
			}
			queue = append(queue, Frame{me, seq, m.Data})
		case Written:
			inFlight--
		case actor.Request:
			port.Close(mic) // no more Chunks; finish what is queued
			boss = m.From
			stopping = true
			actor.Reply(m, true)
		}
		for inFlight < MaxInFlight && len(queue) > 0 {
			actor.Send(w, queue[0])
			queue = pop(queue)
			inFlight++
		}
		if stopping && inFlight == 0 && len(queue) == 0 {
			actor.Send(w, Finish{boss, micDropped, queueDropped})
			return
		}
	}
}
```

The writer appends each frame to a file and acknowledges it:

```go
func writer(path string) {
	f, e := fs.Open(path, fs.Write)
	if e != fs.OK {
		actor.Exit(actor.Failed + int(e))
	}
	frames, n := 0, 0
	for {
		switch m := actor.Receive().(type) {
		case Frame:
			if e := f.Write(m.Data); e != fs.OK {
				actor.Exit(actor.Failed + int(e)) // a supervisor decides what next
			}
			frames++
			n += m.Data.Len()
			actor.Send(m.From, Written{m.Seq})
		case Finish:
			f.Close()
			actor.Send(m.Boss, Stats{frames, n, m.MicDropped, m.QueueDropped})
			return
		}
	}
}
```

`main` spawns both with the limits computed below, records for 1.9 s,
stops the capture with a `Call` and prints what arrived (the whole
program is the book example `21-pipeline`):

```text
capture MaxMemory 18800 writer MaxMemory 9016
frames 15 bytes 24000 mic dropped 0 queue dropped 0
file size 24000
```

1500 ms of sound at 8000 samples a second is 15 frames of 100 ms, 1600
bytes each, and all of them arrived. Two details matter:

- `pop` clears the slot it frees (`q[len(q)-1] = Frame{}`), so the queue's
  array does not keep a frame alive after it was sent.
- The writer is where failures happen (a full card, a removed card). It
  exits with `actor.Failed` plus the storage error; under a supervisor
  it restarts with a new file, and the capture should monitor it and treat
  frames still in flight as lost. `gogo/sound` (section 16.2) is a
  one-actor recorder that stops cleanly on a storage error instead.

### Memory arithmetic for frames

Section 16.1 gives the microphone's charge. With `F = RateHz * FrameMs /
2000` words per frame (a word is 4 bytes), the owner pays `F` for the
port's frame buffer and `F + 11` for every `Chunk` waiting in its mailbox.
To ride out a stall of `S` ms, during which `ceil(S / FrameMs)` more frames
arrive, the owner needs:

```
heap + F + (ceil(S / FrameMs) + 1) * (F + 11)   words
```

A stall is not only the owner's own slowness: a storage call in *any*
actor pauses the whole VM (section 21.8), and when it ends the driver
delivers the frames it buffered all at once. At 16 kHz and 100 ms frames
(F = 800), a 300 ms stall needs heap + 4044 words, about 16 KB plus the
heap: more than a board's default limit by itself. That is why
`gogo/sound` spawns its recorder with `MaxMemory`, and why the capture
actor above does too:

```go
// captureMemory, in bytes: the mic's buffer and the Chunks that pile up
// in a stall of StallMs (section 16.1), plus the frames capture keeps
// queued (F words each, plus 8 for the Frame, the Bytes header and the
// queue slot).
func captureMemory() int {
	f := frameWords()
	stall := (StallMs+FrameMs-1)/FrameMs + 1
	return (HeapWords + f + stall*(f+11) + MaxQueued*(f+8)) * 4
}

// writerMemory, in bytes: the frames in flight in its mailbox and the one
// it holds (F words each, plus 10 for the Frame and the message block).
func writerMemory() int {
	return (HeapWords + (MaxInFlight+1)*(frameWords()+10)) * 4
}
```

With 8 kHz, 100 ms frames (F = 400), a 300 ms stall, 1024 words of heap
and 4 queued frames, that is 18 800 bytes for capture and 9016 for the
writer: about 28 KB of the board's 160 KB for 8 kHz sound. At 16 kHz,
double the frame terms. The numbers are estimates; check them with
`actor.Info` under load and leave a margin.

### What a big frame costs

A frame in flight is charged, in full, to every actor that holds it, the
ones that have it waiting in their mailbox included (section 21.2). So
each place a frame can be (the port's buffer, a mailbox, a queue, a file
write in progress) is a term in the sum, and each extra stage or extra
frame in flight adds a whole frame. When the sum does not fit:

- **A `Chunk` that does not fit its owner** is dropped by the port and
  counted in the next `Chunk`'s `Dropped`. The owner keeps running.
- **Frames forwarded by actors pile up** in the next actor's mailbox.
  That is ordinary messaging: over its limit the receiver exits with
  `MailboxOverflow`, and its supervisor restarts it empty. The window of
  section 21.3 prevents it.
- **Too many frozen frames held at once**: an actor holding a 17th
  off-heap `Bytes`, or a VM with all 64 in use, fails the allocation with
  `OutOfMemory` (and a stream `Out` that needs a handle gets `Done{-1}`).

### Latest frame wins

For live data, a new frame is worth more than an old one. Design so that
when something falls behind, old frames are dropped, not queued:

- **Keep few frames in flight**: two at a stage is enough to overlap work.
  More only adds latency and memory.
- **Release each frame early**: write or send it, then drop the
  reference; do not keep "the last frame" in a variable while waiting.
- **Let the driver drop.** A port drops and counts when its owner is
  full; that is the cheapest place to lose data, since nothing was copied.
  An actor that hoards frames to avoid loss runs out of memory instead,
  and then loses everything.
- **Count and report drops** (`Dropped`, your own counters), so a slow
  stage shows up in the logs instead of as a mysterious gap.

### Cameras

gogo has **no camera port** today. A camera driver would follow the same
stream protocol: each frame (a JPEG, say) a `Chunk`, with `Seq`, `TimeMs`
and `Dropped`, and frames over the owner's quota dropped and counted.

The arithmetic is harder than for audio. A 10 to 30 KB JPEG frame is
larger than a board's default per-actor limit of 16 KB by itself, and is
a large share of the 160 KB all actors share. Every holder is charged the
whole frame, so a design with one frame in the port's buffer, one waiting
and one being sent already needs 30 to 90 KB. Such a design needs very
few frames in flight (one or two), raised `MaxMemory` limits for the
actors that touch frames, and as few stages as possible.

Keeping frames in driver memory (the StickS3's PSRAM) and handing actors
only a handle to them is a possible future design. It does not exist.

### On a board

The same pipeline runs on a board with these changes: `/sd/...` instead of
`/flash/...` for long recordings on the Cardputer ADV; 16 kHz for speech;
the limits recomputed for that rate. Timing differs: the host's simulated
microphone delivers each script line's sound as a burst at the next poll
(section 16.5), while a board delivers 10 frames a second, in a burst only
after a stall. Run it with `build/gogovm_mcu` to check the memory limits,
and on the board to check the timing.

## 21.10 Streaming TCP and HTTP

Network connections are stream ports too, with the same rules, and a few
of their own.

### TCP as a stream port

A `"tcp"` connection (section 10.7) speaks the stream protocol:

- **Bytes in** are `port.Chunk`s, in order, and `Dropped` is always 0:
  instead of dropping, the stack acknowledges a segment only once its
  `Chunk` is in the owner's mailbox, so a slow reader slows the sender
  down. The owner is never killed by its own connection.
- **Bytes out** are `port.Out`s, each answered by a `port.Done` once the
  peer has acknowledged all of it. **At most two `Out`s may be in flight**
  (an `Out` whose `Done` is still owed counts). A third is refused with
  `Done{Seq: -1}` and **not sent**; the connection carries on. (Older
  versions closed the connection instead; that changed with the stream
  protocol.) So count `Done`s, and keep what you could not send yet in a
  `Buffer`. An empty `Out`, one after `Shutdown`, and one on a connection
  that has ended get `Done{-1}` too.
- **`port.Closed{Reason}`** ends the stream. `net.Normal` (0) means only
  that the peer has finished sending: you may still send, then `Shutdown`.
  Any other reason means the connection is gone: `net.Timeout`,
  `net.Refused`, `net.Reset`, `net.NoRoute`, and for TLS `net.BadCert`,
  `net.NoTime` and `net.TLS`. They are the shared codes of `gogo/errors`
  (`net.Timeout` is `errors.Timeout`), the same numbers `http.Upload`
  returns. `Out`s still in flight get `Done{-1}` before such a `Closed`.
  After it, close the port.
- **One owner per socket.** A connection's events go to the actor that
  owns it, and the connection dies with that actor. An accepted connection
  is paused until `port.Handoff` gives it to the actor that will serve it
  (section 7.4).

TCP is a byte stream. A `Chunk` holds whatever the network delivered: half
a line, two lines and a bit, one byte. **Never treat a `Chunk` as a
message.** Join the bytes, parse complete frames (a line, a length-prefixed
record) and keep the rest for the next `Chunk`. Bound the rest: a peer
that never sends the end of a frame must not grow your memory without
limit.

### The server pattern

The listener accepts and hands off; one actor per connection does the
work. A client that sends garbage crashes only its own actor, and a
supervisor keeps the listener alive:

```go
// listener accepts connections and gives each to a new handler actor.
func listener(portNum int) {
	_, err := port.Open("tcp_listen", net.Listen{Port: portNum})
	if err != nil {
		actor.Exit(actor.Failed + int(errors.Busy)) // the supervisor retries
	}
	live := 0
	for {
		switch m := actor.Receive().(type) {
		case net.Accepted:
			conn := m.Conn
			if live >= MaxConns {
				port.Close(conn) // refuse: reset
				continue
			}
			h := actor.SpawnOpt(func() { serveConn(conn) }, actor.Options{MaxMemory: ConnMemory})
			if !port.Handoff(conn, h) {
				port.Close(conn)
				actor.Kill(h, actor.Killed)
				continue
			}
			actor.Monitor(h)
			live++
		case actor.DownMsg:
			live-- // a handler ended, however it ended
		}
	}
}
```

The handler joins `Chunk`s, answers complete lines, and keeps at most two
`Out`s in flight:

```go
// flush sends what waits in out while fewer than two Outs are in flight,
// and returns the new count.
func flush(conn actor.PID, out bytes.Buffer, inFlight int) int {
	if inFlight < 2 && out.Len() > 0 {
		actor.Send(conn, port.Out{Data: out.Freeze()})
		inFlight++
	}
	return inFlight
}

// serveConn owns one connection.
func serveConn(conn actor.PID) {
	var partial bytes.Bytes // the start of a line not finished yet
	in := bytes.NewBuffer(256)
	out := bytes.NewBuffer(256)
	inFlight := 0
	peerDone := false
	for {
		switch m := actor.Receive().(type) {
		case port.Chunk:
			in.WriteBytes(partial)
			in.WriteBytes(m.Data)
			data := in.Freeze()
			start := 0
			for i := 0; i < data.Len(); i++ {
				if data.At(i) == '\n' {
					out.WriteString(answer(data.Slice(start, i).String()))
					start = i + 1
				}
			}
			partial = data.Slice(start, data.Len())
			if partial.Len() > MaxLine {
				port.Close(conn) // no Shutdown: the connection is reset
				return
			}
		case port.Done:
			inFlight--
		case port.Closed:
			if m.Reason != net.Normal { // reset, timeout, link lost
				port.Close(conn)
				return
			}
			peerDone = true // the peer finished sending; answer what we have
		}
		inFlight = flush(conn, out, inFlight)
		if peerDone && inFlight == 0 && out.Len() == 0 {
			actor.Send(conn, net.Shutdown{})
			port.Close(conn) // after Shutdown the stack completes the close
			return
		}
	}
}
```

`gogo run` has no network (section 10.1), so on the host the example's
`main` plays one connection itself: a stand-in actor sends the handler
the messages a `"tcp"` port would (`Chunk`s splitting the lines in odd
places: `"pi"`, `"ng\nhello wor"`, `"ld\nping\n"`, `"bye"`, then
`Closed{Normal}`), answers each `Out` with a `Done`, and prints what it
was sent:

```text
no network: one connection through a stand-in
sent: pong
sent: echo: hello world
sent: pong
shutdown after 2 Outs
handler ended, reason 0
```

The three answers went out in two `Out`s (the second carried two lines);
the unfinished `bye` was never answered, because no newline ended it. On a
board, `main` starts the listener under a supervisor instead. The real
listener and port path is tested too: `tests/net` runs this example on the
simulated network (`TestBookLineServer`), with two clients at once, lines
split over segments, the close, and a line over `MaxLine` that resets only
its own connection.

Limits that shape a server:

- **Connections are few.** The stack has 4 TCP connection slots, and
  incoming connections never take the last one, so at most 3 clients are
  served at once (and the device can always dial out). Each VM also has
  only 16 port slots in all, shared with files, pins and the rest.
- **Give each handler a limit** (`ConnMemory` above) and size it: its
  heap, the `Chunk`s the stack may deliver before it reads them, and what
  it buffers. The cap times the limit is the server's memory budget.
- **Bound everything a client controls**: line length (`MaxLine`), request
  size, the time a connection may stay idle.
- **Close properly.** `Shutdown` then `port.Close` lets the stack finish the
  close on its own; `port.Close` alone (or the handler's exit) resets the
  connection.

`http.Serve` (section 10.5) is this pattern already: a listener and one
actor per request, with the head limited to 2 KB, the body to 8 KB and the
wait to 10 s.

### HTTP bodies

In `gogo/http`, bodies are single `bytes.Bytes` values with fixed limits,
not streams: a response body is read into memory whole, up to
`http.MaxBody` (8 KB; up to 64 KB with `http.Do`), and marked `Truncated`
beyond it; the server takes request bodies up to 8 KB. The whole body is
charged to the actor that gets it, so a caller that wants a 64 KB
response needs a limit to match (section 10.4). There is no streaming
download yet; for a large download, use the `"tcp"` or `"tls"` port and a
framing of your own (chapter 17's warning applies: over TLS, frame the
data, never "read until close").

**Uploads do stream.** `http.Upload` (section 17.4) reads a file 4 KB at
a time and keeps at most two writes in flight, so a 1 MB recording on
`/sd` goes up with the same memory as a 16 KB one, and is never loaded
whole. Upload recordings that way, never with `http.Post` of a file read
into memory. Section 17.4's example shows the calls and the error cases.

### Keep-alive and TLS cost

TLS is the most expensive thing a board does on the network:

- A handshake takes about 2.5 s on the StickS3, most of it sliced so other
  actors keep running, but one step of about **200 ms pauses the whole
  VM** (section 21.8).
- A TLS connection charges its owner about **23 KB** (peak about 34 KB
  during the handshake), more than a board's default actor limit; that is
  why `gogo/http` runs requests in actors of its own with a larger limit.
- A board has **one TLS connection at a time**. A second https request
  fails at once while one is open.

So: talk to a server you poll often through one keep-alive `http.Client`
(section 17.3), which pays for the handshake once; upload through the same
client (`http.ClientUpload`) when you have one, since a separate `Upload`
needs the one TLS connection; close a client you no longer need
(`http.CloseClient`), since while it holds its connection no other https
request can open one; and do not start https work while something
real-time is running.

### Timeouts, retries and partial failure

- **Every network wait has a timeout**: `http.Get` and `ClientDo` take one,
  a `net.Dial` has `TimeoutMs`, `Call`s to network actors have theirs.
  Give https requests 10 s or more: the handshake alone takes seconds.
- **A transfer can fail in the middle.** A connection ends with
  `Closed{Reason}`; `http.Upload` returns that reason as its error, and
  `ClientDo` fails without retrying, because the server may already have
  acted on the request. Retrying is the program's decision.
- **Retry what may pass, with backoff.** A timeout, a reset, no route or no
  answer may pass; a missing file, a bad URL or a bad certificate will
  not. Wait between attempts, doubling the wait, and give up after a few.
- **Restart rather than repair.** An upload actor that fails can simply
  exit; its supervisor (or its caller) starts a new one, which begins the
  upload again from the file. `http.Upload` follows its caller: if the
  caller dies, the upload is reset at once, so the next attempt finds the
  TLS connection free.
- **Set the clock before TLS.** Certificate checks need the date, and a
  board does not know it after boot: call `sntp.Sync` once the network is
  up, or every https request fails (`net.NoTime`, section 17.1).

```go
// transient says whether trying again later may help.
func transient(e errors.Code) bool {
	return e == errors.Timeout || e == errors.IO || e == errors.Reset ||
		e == errors.Refused || e == errors.NoRoute
}

// uploadWithRetry tries up to attempts times; waitMs doubles after each
// failure. It returns the server's status, or 0 and the last error.
func uploadWithRetry(url, path, token string, attempts, waitMs int) (int, errors.Code) {
	for i := 1; ; i++ {
		status, err := http.Upload(url, path, "audio/wav", token)
		if err == errors.OK && status < 500 {
			return status, err // 2xx, or a 4xx that retrying will not fix
		}
		if err != errors.OK && !transient(err) {
			return 0, err
		}
		if i == attempts {
			return status, err
		}
		println("attempt", i, "failed, error", int(err), "status", status, "- retry in", waitMs, "ms")
		actor.Sleep(waitMs)
		waitMs *= 2
	}
}
```

On the host there is no network, so every real attempt fails with
`errors.IO` (8), and the missing file fails at once with
`errors.NotFound` (1) without a retry (the `dns:` line is the resolver
saying there is no network):

```text
missing file: 0 1 (not retried)
dns: net.Subscribe failed (no network or 4 subscribers): retrying on each lookup
attempt 1 failed, error 8 status 0 - retry in 100 ms
attempt 2 failed, error 8 status 0 - retry in 200 ms
attempt 3 failed, error 8 status 0 - retry in 400 ms
upload: 0 8 gave up after 700 ms of waits: true
```

A retried upload may reach the server twice (the first attempt may have
arrived before the connection broke), so make uploads safe to repeat: name
the file in the URL, so a second copy replaces the first.

## 21.11 Checklist

Review a design against these:

**Memory**

- Does every working actor have a `MaxMemory`, measured with
  `actor.Info`? Does the program run with `build/gogovm_mcu`?
- Do the limits of actors busy at the same time fit in 160 KB?
- Are messages small, and is bulk data a frozen `bytes.Bytes`?
- Does any actor hold many large `Bytes` at once (16 per actor, 64 per
  VM), or keep one alive in a variable while it waits?

**Flow**

- Can any producer outrun its consumer? Is there a window, a `Call`, a
  pull, or a reduction at the source?
- Does every `Call` have a timeout, and is `ok == false` handled?
- Do requests flow one way, with no `Call` cycles?
- Does each actor handle every message type it can receive, in every
  state, including late answers and messages from other senders?

**Failure**

- Is every long-lived actor under a supervisor, or monitored?
- Is state that must survive a restart in `gogo/kv` or a file, written
  before it is acknowledged, and reloaded at start?
- Do restarted services re-register their names and re-open their ports?

**Time**

- Do loops over thousands of elements per second run in C, not in gogo?
- Are big heaps, storage calls and https requests kept away from
  real-time work?

**Streams**

- Does the data plane stay in the driver, with actors seeing whole frames?
- Is `MaxMemory` sized for the longest stall, with section 16.1's formula?
- Are few frames in flight (two per stage), released early, with drops
  counted and reported?
- Are `Out`s limited to two in flight, with the rest buffered?

**Network**

- Are frames parsed from joined `Chunk`s, never from one `Chunk`, with
  bounded partial data?
- Does each connection have its own actor, a memory limit, and a cap on
  how many run at once (at most 3 inbound)?
- Is every `Closed` reason handled, `Normal` (the peer is done) apart from
  the rest (the connection is gone)?
- Are large files uploaded with `http.Upload`, not read into memory?
- Is there one keep-alive `http.Client` per server, closed when done, and
  only one TLS connection at a time?
- Does every network wait have a timeout, are only transient failures
  retried, with backoff, and is the clock set before TLS?
