# 4. Actors

An **actor** is a lightweight process with three things of its own: a
**heap** (its memory), a **mailbox** (a queue of incoming messages) and a
**PID** (its address). Actors share nothing. The only way one actor affects
another is to send it a message.

If you know Go, think of an actor as a goroutine that owns all its data and
has exactly one channel, its mailbox, that anyone with its PID can send
to. If you know Erlang, this is the Erlang process model with Go syntax:
`spawn` is `actor.Spawn`, `!` is `actor.Send`, and `receive` is
`actor.Receive` plus a type switch.

Everything in this chapter is in the `gogo/actor` package.

## 4.1 Spawn, Send, Receive

```go
func Spawn(f func()) PID
func Send(to PID, msg any)
func Receive() any
func Self() PID
```

`Spawn` starts a new actor running `f` and returns its PID at once; the
new actor runs when the scheduler gets to it. `Send` puts a copy of `msg`
in the mailbox of `to` and returns immediately; it never blocks and never
fails (a message to an actor that no longer exists is dropped). `Receive`
takes the oldest message from your own mailbox, waiting if there is none.

An actor ends when its function returns. The typical actor is a loop
around `Receive` that keeps its state in local variables:

```go
// An actor that owns state: a counter. Other actors change it and read it
// only by sending messages.
package main

import "gogo/actor"

type Add struct{ N int }

type Get struct{ From actor.PID }

type Value struct{ N int }

type Stop struct{}

func counter() {
	total := 0
	for {
		switch m := actor.Receive().(type) {
		case Add:
			total += m.N
		case Get:
			actor.Send(m.From, Value{total})
		case Stop:
			println("counter stopping at", total)
			return // returning from the entry function ends the actor
		default:
			println("counter: unknown message dropped")
		}
	}
}

func main() {
	c := actor.Spawn(counter)
	for i := 1; i <= 10; i++ {
		actor.Send(c, Add{i})
	}
	actor.Send(c, "hello?") // any value can be sent; the counter drops it
	actor.Send(c, Get{actor.Self()})
	switch v := actor.Receive().(type) {
	case Value:
		println("total", v.N)
	}
	actor.Send(c, Stop{})
}
```

```text
counter: unknown message dropped
total 55
counter stopping at 55
```

Points to notice:

- The **state** (`total`) is an ordinary local variable. No other actor can
  touch it, so there is no locking. Other actors change it by sending `Add`
  and read it by sending `Get`.
- Each message type is its own struct. Using a struct per kind of message
  (instead of, say, strings) lets the type switch pick them apart, and lets
  the Go type checker catch typos.
- A request that needs an answer carries the PID to answer to (`Get{From}`).
  Section 4.3 shows a more convenient way.
- The `default` case drops messages the actor does not understand. Without
  it, unknown messages would be dropped too (`Receive` already took them
  out of the mailbox); the `default` just makes it visible.
- Messages from one sender arrive in the order they were sent.

Every message is copied into the receiver's memory. Chapter 6 explains why
and what it costs; for now just remember that the receiver can never change
the sender's data.

### Main is an actor too

`main` runs in the first actor. When `main` returns, only that actor ends;
other actors keep running. The VM exits when no actor can run any more, no
timer is pending and no port can deliver an event. On a board the VM then
prints `gogo: vm exited` and idles.

### Scheduling

The VM runs one actor at a time on a single scheduler. An actor runs until
it waits (in `Receive`, `Sleep`, `Call` ...) or until it has used up its
time slice: every function call and every loop iteration costs one
*reduction*, and after 1000 the actor is put at the back of the queue. So a
busy loop cannot freeze the other actors, even `for { n++ }`.

### The zero PID

A PID variable that was never set holds the zero PID. You can also write
it as `actor.PID{}` or `actor.NoPID` (a read-only package-level
variable). It names no actor, so it is a safe "nobody yet" value: compare
with it (`if w == actor.NoPID`) to see whether a PID was set. Using it is
never a crash: a `Send` to it is dropped, a `Call` to it fails at once
with `errors.NoProc` (4.3), and `Monitor` or `Link` of it behaves as for
an actor that has exited (a `DownMsg` or exit signal with reason
`actor.NoProc` arrives at once; chapter 5). `Whereis` of an unknown name
and a failed `port.Open` return it.

## 4.2 Type switches: reading messages

`Receive` returns `any`, and a type switch finds out what it holds. The
case types can be any of your struct types, `int`, `int64`, `float32`,
`float64`, `bool`, `string`, `actor.PID`, `actor.Ref` and `bytes.Bytes`, and the struct types
of the gogo packages (`actor.ExitMsg`, `gpio.Edge`, `kbd.Key`, ...):

```go
// The type switch is how an actor tells messages apart. Cases can be
// struct types, int, int64, bool, string, actor.PID, actor.Ref and
// bytes.Bytes; default catches the rest.
package main

import "gogo/actor"

type Celsius struct{ Deg int }

type Reset struct{}

func describe(m any) {
	switch v := m.(type) {
	case Celsius:
		println("temperature", v.Deg)
	case Reset:
		println("reset")
	case int:
		println("int", v)
	case int64:
		println("int64", v)
	case string:
		println("string", v)
	case bool:
		println("bool", v)
	case actor.PID:
		println("a pid")
	default:
		println("something else")
	}
}

func main() {
	me := actor.Self()
	for _, m := range []any{Celsius{21}, Reset{}, 7, int64(7), "hi", true, me, []int{1}} {
		describe(m)
	}
}
```

```text
temperature 21
reset
int 7
int64 7
string hi
bool true
a pid
something else
```

`int` and `int64` are different cases: an `int64` put into `any` stays an
`int64`. So do a `byte` and a value of a named basic type such as `type
Celsius int`: each keeps its own type in an `any` (it is boxed), so `case
Celsius` matches a `Celsius` and not an `int`. Slices can be sent but cannot
be a case; put them in a struct.

When only one type is expected, a type assertion is shorter: `t :=
m.(Celsius)` takes the value out, and crashes the actor with a panic if `m`
holds another type; `t, ok := m.(Celsius)` does not crash but sets `ok` to
false (and `t` to the zero value).

> **Note.** Erlang's `receive` can wait for a message matching a pattern
> and leave the others in the mailbox. gogo's `Receive` always takes the
> oldest message. The one exception is `Call` (next section), which waits
> for its own reply and leaves everything else in place. This keeps the VM
> simple and receiving cheap.

## 4.3 Call and Reply

Asking a question and waiting for the answer is so common that gogo has
it built in:

```go
func Call(to PID, req any, timeoutMs int) (reply any, err error)
func Reply(req Request, reply any)

type Request struct{ From PID; Ref Ref; Body any }
```

`Call` wraps `req` in an `actor.Request` with a fresh unique `Ref`, sends
it, and waits for a reply carrying that `Ref`, at most `timeoutMs`
milliseconds. Other messages that arrive meanwhile stay in the mailbox.
`err` is `nil` when the reply came; otherwise `reply` is `nil` and `err`
is a `gogo/errors` code that says why:

| `err` | Why | When |
|---|---|---|
| `errors.Timeout` | no reply within `timeoutMs` | after the timeout (at once for `timeoutMs <= 0` with no reply yet) |
| `errors.NoProc` | `to` is no live actor: it has exited, never existed, is the zero PID or a port | at once |
| `errors.NoConnection` | `to` is on another node that is not connected, or went away while you waited (4.8, chapter 20) | at once |

Compare with `==` (`err == errors.Timeout`) or print `err.Error()`. The
`req` of a failed Call is still checked as a `Send`'s message is, so one
that cannot be sent (a closure, a port's PID to another node, ...) stops
the caller with `actor.BadMsg`.
The server receives an `actor.Request`, looks at its `Body`, and answers
with `actor.Reply`:

```go
// Call and Reply: a request-reply server holding a small key-value table.
package main

import (
	"gogo/actor"
	"gogo/errors"
)

type Put struct {
	Key   string
	Value int
}

type Lookup struct{ Key string }

type Found struct {
	Value int
	OK    bool
}

type entry struct {
	Key   string
	Value int
}

func store() {
	var tbl []entry
	for {
		switch r := actor.Receive().(type) {
		case actor.Request:
			switch b := r.Body.(type) {
			case Put:
				tbl = put(tbl, b.Key, b.Value)
				actor.Reply(r, true)
			case Lookup:
				v, ok := get(tbl, b.Key)
				actor.Reply(r, Found{v, ok})
			}
		}
	}
}

func put(tbl []entry, k string, v int) []entry {
	for i, e := range tbl {
		if e.Key == k {
			tbl[i] = entry{k, v}
			return tbl
		}
	}
	return append(tbl, entry{k, v})
}

func get(tbl []entry, k string) (int, bool) {
	for _, e := range tbl {
		if e.Key == k {
			return e.Value, true
		}
	}
	return 0, false
}

// lookup wraps the Call in a function with a typed result.
func lookup(s actor.PID, k string) (value int, found bool, err error) {
	reply, err := actor.Call(s, Lookup{k}, 100)
	if err != nil {
		return 0, false, err // errors.Timeout, errors.NoProc, ...
	}
	f, _ := reply.(Found)
	return f.Value, f.OK, nil
}

func main() {
	s := actor.Spawn(store)
	if _, err := actor.Call(s, Put{"led", 1}, 100); err != nil {
		println("put led:", err.Error())
	}
	if _, err := actor.Call(s, Put{"fan", 0}, 100); err != nil {
		println("put fan:", err.Error())
	}
	v, found, _ := lookup(s, "led")
	println("led", v, found)
	v, found, _ = lookup(s, "door")
	println("door", v, found)

	// A message sent to main before a Call stays in the mailbox: Call
	// waits only for its own reply.
	actor.Send(actor.Self(), "kept for later")
	v, found, _ = lookup(s, "fan")
	println("fan", v, found)
	println(actor.Receive())

	// A Call to an actor that never answers times out.
	silent := actor.Spawn(func() { actor.Receive() })
	_, err := actor.Call(silent, Lookup{"x"}, 50)
	println("silent:", err.Error())

	// A Call to an actor that has exited fails at once.
	actor.Kill(silent, actor.Killed)
	_, _, err = lookup(silent, "x")
	println("killed:", err == errors.NoProc, err.Error())
}
```

```text
led 1 true
door 0 false
fan 0 true
kept for later
silent: timed out
killed: true no such actor
```

It is good style to hide each `Call` behind a small function with a typed
result, like `lookup` here, so that the rest of the program never sees
`any`.

> **Note.** If the server dies while you wait, `Call` does not return
> early: it waits until the timeout and fails with `errors.Timeout`.
> Monitor the server (chapter 5) if you need to know sooner. (For a
> server on another board, see 4.8 and 20.5: a Call fails at once with
> `errors.NoConnection` when the server's node goes away.)

## 4.4 Registered names

Passing PIDs around works, but some actors are services that many others
need: a logger, a display, the DNS resolver. Give them a name:

```go
func Register(name string, p PID) error  // errors.NoProc or errors.Exists
func Whereis(name string) (PID, bool)
func Unregister(name string)
```

`Register` fails with `errors.NoProc` if `p` is not a live actor of this
VM (it has exited, is the zero PID, a port or a remote PID; this is
checked first), and with `errors.Exists` if another actor has the name.
`Whereis` has no error, only `ok`: it looks the name up in this VM's own
table, as `v, ok := m[k]` looks in a map, and either the name is there or
it is not. `node.Whereis` (chapter 20), which finds a name on another
node, returns an `error` instead, because it asks over the network and
can fail for the network's reasons (`errors.Timeout`,
`errors.NoConnection`).

```go
// Registered names: find a service by name instead of passing its PID.
package main

import "gogo/actor"

type Log struct{ Line string }

type Flush struct{ From actor.PID }

func logger() {
	n := 0
	for {
		switch m := actor.Receive().(type) {
		case Log:
			n++
			println("log", n, m.Line)
		case Flush:
			actor.Send(m.From, n)
		}
	}
}

// logf sends to the "log" service if it runs.
func logf(line string) {
	if p, ok := actor.Whereis("log"); ok {
		actor.Send(p, Log{line})
	}
}

func main() {
	logf("lost: nobody is registered yet")
	l := actor.Spawn(logger)
	if err := actor.Register("log", l); err != nil {
		println("register:", err.Error())
	}
	err := actor.Register("log", actor.Self())
	println("again:", err.Error()) // errors.Exists: the name is taken

	done := actor.Self()
	for i := 0; i < 2; i++ {
		actor.Spawn(func() {
			logf("from a worker")
			actor.Send(done, true)
		})
		actor.Receive()
	}
	logf("from main")
	actor.Send(l, Flush{actor.Self()})
	println("lines logged:", actor.Receive())

	actor.Unregister("log")
	_, ok := actor.Whereis("log")
	println("after unregister:", ok)
}
```

```text
again: exists already
log 1 from a worker
log 2 from a worker
log 3 from main
lines logged: 3
after unregister: false
```

A name belongs to the actor until it exits or the name is unregistered;
then the name is free again. Only the actor that registered a name can
unregister it; `Unregister` from any other actor does nothing.
A supervisor that restarts a named service can
have the new instance register the same name, so clients that look the
name up each time always find the live one. One actor can hold several
names.

## 4.5 Time

```go
func Now() int64                               // ms since the VM started
func Sleep(ms int)
func ReceiveTimeout(ms int) (msg any, ok bool) // ms == 0: only look
func SendAfter(to PID, msg any, ms int) Ref    // Send's arguments, then the delay
func CancelTimer(r Ref) bool                   // false if it already fired
```

`Sleep` pauses only the calling actor. `ReceiveTimeout` is `Receive` with
a deadline; `ok` is false if nothing came. `SendAfter` arranges for a
message to be delivered later, which is how you build periodic work: on
each tick, arm the next one.

```go
// Time: Now, Sleep, ReceiveTimeout, SendAfter and CancelTimer.
package main

import "gogo/actor"

type Tick struct{ N int }

func main() {
	start := actor.Now()
	actor.Sleep(30)
	println("slept at least 30 ms:", actor.Now()-start >= 30)

	// ReceiveTimeout: wait at most 20 ms. 0 only looks.
	_, ok := actor.ReceiveTimeout(20)
	println("got a message:", ok)

	// SendAfter delivers a message later; timers fire in deadline order.
	me := actor.Self()
	actor.SendAfter(me, "late", 40)
	actor.SendAfter(me, "early", 10)
	t := actor.SendAfter(me, "cancelled", 20)
	println("cancel:", actor.CancelTimer(t))
	println(actor.Receive())
	println(actor.Receive())

	// A periodic tick: re-arm the timer each time it fires.
	actor.SendAfter(me, Tick{1}, 10)
	last := 0
	for last < 3 {
		switch m := actor.Receive().(type) {
		case Tick:
			println("tick", m.N)
			last = m.N
			if m.N < 3 {
				actor.SendAfter(me, Tick{m.N + 1}, 10)
			}
		}
	}
	println("done")
}
```

```text
slept at least 30 ms: true
got a message: false
cancel: true
early
late
tick 1
tick 2
tick 3
done
```

## 4.6 What a spawned closure takes with it

`Spawn` takes a function value, so you can pass a closure that captures
arguments for the new actor: `actor.Spawn(func() { worker(me, 3) })`. The
captured values are **copied** into the new actor's heap when it starts.
Changing them in the child does not change them in the parent:

```go
// A spawned closure gets copies of what it captures: the new actor shares
// no memory with its parent.
package main

import "gogo/actor"

func main() {
	me := actor.Self()
	data := []int{1, 2, 3}
	actor.Spawn(func() {
		data[0] = 100 // changes the child's copy only
		actor.Send(me, data[0])
	})
	println("child saw", actor.Receive())
	println("parent still has", data[0])

	// Each actor has its own PID; Self tells an actor its own.
	child := actor.Spawn(func() {
		actor.Send(me, actor.Self())
	})
	switch p := actor.Receive().(type) {
	case actor.PID:
		println("same pid:", p == child, p == me)
	}
}
```

```text
child saw 100
parent still has 1
same pid: true false
```

A captured variable that the closure and its maker both change (chapter
3) is shared within one actor; a spawned closure takes a copy of it with
it, so from then on the child's variable is its own.

`SpawnLink` and `SpawnOpt` are variants of `Spawn` that link the new actor
to the caller and set its memory limit; they come up in the next two
chapters.

## 4.7 Designing with actors

A few habits make actor programs easy to follow:

- **One actor per resource or activity.** The actor that opened a pin
  handles that pin; the actor that owns the screen draws on it. Others send
  it messages.
- **Messages are structs with exported, meaningful names**, one per kind
  of request or event. Put the reply-to PID in the message, or use `Call`.
- **Wrap the protocol in functions.** Callers should call `lookup(s, k)`,
  not build `Request`s.
- **Keep handlers short.** While an actor handles one message, its mailbox
  fills. Give slow work to another actor.
- **Do not handle every error.** If an actor gets into a state it does not
  understand, let it exit and let a supervisor start a fresh one. The next
  chapter shows how.

## 4.8 Actors on other boards

Everything in this chapter also works between boards. With `gogo/node`
(chapter 20), a PID can name an actor on another board or on a server, and
`Send`, `Call`/`Reply`, `Monitor` and `Link` work on it unchanged. A
message to a remote actor is copied over the network as a local one is
copied into the receiver's heap, with a few limits on what may cross
(20.4): no func values, pointers, maps or `bytes.Buffer`, and at most
about 4 KB encoded. When an actor's whole node goes away, its links and
monitors fire with the reason `actor.NoConnection` (chapter 5, 20.5).

This is the client of
[examples/nodehello](../../examples/nodehello/main.gg) joining a server
node, finding its exported counter and calling it:

```go
	if err := node.Connect(addr, "client"); err != nil {
		println("connect:", err.Error())
		return
	}
	counter, err := node.Whereis("server", "counter", 2000)
	if err != nil {
		println("whereis:", err.Error())
		return
	}
	println("client: found counter", counter)
	ref := actor.Monitor(counter)
	for i := 1; i <= 5; i++ {
		total, err := actor.Call(counter, Add{N: i}, 2000)
```

The counter on the server is an ordinary `Call` server like the one in
4.3; `tests/dist` runs the program as a server and clients on separate
host VMs. Chapter 20 shows how to set up the server node and the shared
token, and covers delivery, limits and security.
