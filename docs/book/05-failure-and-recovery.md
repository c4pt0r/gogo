# 5. Failure and Recovery

Programs fail. A sensor returns garbage, a parser meets input nobody
thought of, a counter overflows into an index. In most embedded systems a
fault like that resets the whole device, or worse, corrupts memory and
carries on.

gogo takes Erlang's approach: **make failure small, visible and
recoverable.** A fault ends only the actor it happened in. The actors that
care are told why. A supervisor starts a fresh copy. Because the failed
actor's memory, pins and files are released when it ends, the fresh copy
starts from a clean state. This is "let it crash": instead of guarding
against every possible error inside an actor, you arrange for the system
around it to recover.

## 5.1 A crash ends one actor

```go
// A crash ends only the actor that crashed. Its neighbours keep running.
package main

import "gogo/actor"

func divide(a, b int) int { return a / b }

func main() {
	me := actor.Self()
	for _, d := range []int{2, 0, 5} {
		actor.Spawn(func() {
			actor.Send(me, divide(100, d)) // d == 0 crashes this actor only
		})
	}
	// Two answers arrive; the third worker crashed instead of answering.
	for i := 0; i < 2; i++ {
		println("answer", actor.Receive())
	}
	_, ok := actor.ReceiveTimeout(50)
	println("third answer:", ok)
	println("main is fine")
}
```

Standard output:

```text
answer 50
answer 20
third answer: false
main is fine
```

Standard error:

```text
gogo: actor <pid 2> crashed: divzero in divide
```

The worker that divided by zero ended; the other two answered; `main`
carried on. The VM printed a **crash report** on standard error (on a board,
in red on the screen):

```text
gogo: actor <pid N> crashed: <kind>[ in <function>]
```

`<pid N>` is the actor's slot number. Slots are reused, so two different
actors over time can show the same number, and the zero PID and the main
actor both print as `<pid 0>`. `<function>` is where a runtime error
happened; closures are named `main.main.func1` and so on.

## 5.2 Exit reasons

Every actor ends with a **reason**:

```go
type Reason struct {
	Code int    // a reason code
	Func string // function a runtime error happened in, else ""
}
```

| Code | Constant | Cause | Crash report |
|---|---|---|---|
| 0 | `actor.Normal` | the entry function returned | none |
| 1 | `actor.Killed` | `actor.Kill(p, actor.Killed)` | `killed` |
| 2 | `actor.OutOfMemory` | over its memory limit (chapter 6), or the whole VM ran out | `oom` |
| 3 | `actor.MailboxOverflow` | queued messages took it over its limit | `mailbox_overflow` |
| 4 | `actor.NoProc` | linked to an actor that did not exist | `noproc` |
| 5 | `actor.DivZero` | integer division or `%` by zero | `divzero` |
| 6 | `actor.BadCode` | index or slice out of range, nil function call, unusable zero `Buffer`, ... | `badcode` |
| 7 | `actor.Shutdown` | asked to stop (supervisors use this) | none |
| 8 | `actor.BadShift` | a negative shift count | `badshift` |
| 9 | `actor.BadMsg` | sent a module's own value to another module (chapter 14) | `badmsg` |
| 10 | `actor.Panic` | `panic(v)`, a failed type assertion, a method call on a nil interface | `panic`, after `panic: v` |
| 11 | `actor.NoConnection` | linked to an actor on another node that became unreachable ([20.5](20-distributed.md#205-when-a-node-goes-away)) | `noconnection` |
| 12 | `actor.Nil` | a nil pointer dereference, a write to a nil map | `nil` |
| 64..99 | `actor.Fail(code)` | a failure a shared error code describes: `actor.Failed` (64) + the code (appendix C), as `http.ListenFailed` is `actor.Fail(errors.Busy)`, 69 | `exit(<code>)` |
| ≥ 100 | your own | `actor.Exit(code)` or `actor.Kill(p, code)` | `exit(<code>)` |

`Normal` and `Shutdown` are the two "expected" endings; everything else
counts as abnormal. Here is how each one is produced and what a monitor
(section 5.4) sees:

```go
// Every way an actor can end, and the reason code a monitor sees for it.
package main

import "gogo/actor"

func watch(name string, f func()) {
	p := actor.Spawn(f)
	actor.Monitor(p)
	switch m := actor.Receive().(type) {
	case actor.DownMsg:
		println(name, "-> code", m.Reason.Code, "func ["+m.Reason.Func+"]")
	}
}

func deep(n int) int { return deep(n+1) + 1 }

func main() {
	watch("return", func() {})
	watch("exit(150)", func() { actor.Exit(150) })
	watch("divide by zero", func() {
		zero := 0
		println(1 / zero)
	})
	watch("index out of range", func() {
		xs := []int{1, 2, 3}
		i := 3
		println(xs[i])
	})
	watch("negative shift", func() {
		n := -1
		println(1 << n)
	})
	watch("nil func", func() {
		var f func()
		f()
	})
	watch("endless recursion", func() { println(deep(0)) })
	watch("panic", func() { panic("boom") })
	watch("killed", func() {
		actor.Kill(actor.Self(), actor.Killed)
	})
}
```

```text
return -> code 0 func []
exit(150) -> code 150 func []
divide by zero -> code 5 func [main.main.func3]
index out of range -> code 6 func [main.main.func4]
negative shift -> code 8 func [main.main.func5]
nil func -> code 6 func [main.main.func6]
endless recursion -> code 2 func [deep]
panic -> code 10 func [main.main.func8]
killed -> code 1 func []
```

Standard error:

```text
gogo: actor <pid 1> crashed: exit(150)
gogo: actor <pid 1> crashed: divzero in main.main.func3
gogo: actor <pid 1> crashed: badcode in main.main.func4
gogo: actor <pid 1> crashed: badshift in main.main.func5
gogo: actor <pid 1> crashed: badcode in main.main.func6
gogo: actor <pid 1> crashed: oom in deep
panic: boom
gogo: actor <pid 1> crashed: panic in main.main.func8
gogo: actor <pid 1> crashed: killed
```

Note that endless recursion is not a special error: the stack lives in the
actor's memory, so it simply runs into the memory limit (`oom`).

`panic(v)` is Go's: it prints `panic: ` and the value on standard error
and ends the actor, here with the reason `actor.Panic`. There is no
`recover`. A panic is a crash like the others, so the links, monitors and
supervisors of the next sections handle it; that is gogo's answer to
"something that should never happen just did".

## 5.3 Links

A **link** joins two actors' fates. When one exits with an abnormal reason,
the other receives an *exit signal* and, unless it traps exits, exits too
with the same reason. Links are two-way.

```go
func SpawnLink(f func()) PID  // spawn and link in one step
func Link(p PID)
func Unlink(p PID)
```

Use links for actors that only make sense together: a connection handler
and the parser it started, a worker and its helper. If one fails, both
should be restarted together.

```go
// Links tie actors' fates together: when one exits abnormally, the actors
// linked to it exit too, with the same reason.
package main

import "gogo/actor"

type Hello struct{ Who string }

func part(name string, boss actor.PID, crash bool) {
	actor.Send(boss, Hello{name})
	if crash {
		zero := 0
		println(1 / zero)
	}
	for {
		actor.Receive()
	}
}

func main() {
	me := actor.Self()
	// a and b are linked: SpawnLink inside a links b to a.
	a := actor.Spawn(func() {
		actor.SpawnLink(func() { part("b", me, true) })
		part("a", me, false)
	})
	ref := actor.Monitor(a)
	for i := 0; i < 2; i++ {
		switch m := actor.Receive().(type) {
		case Hello:
			println("started", m.Who)
		}
	}
	// b crashed; a, linked to it and not trapping exits, went down too.
	switch m := actor.Receive().(type) {
	case actor.DownMsg:
		println("a went down with b:", m.Ref == ref, m.Reason.Code == actor.DivZero)
	}

	// A normal exit does not take linked actors down.
	c := actor.Spawn(func() {
		actor.SpawnLink(func() {}) // returns at once: Normal
		actor.Sleep(20)
		actor.Send(me, "c survived its partner's normal exit")
	})
	_ = c
	println(actor.Receive())
}
```

```text
started a
started b
a went down with b: true true
c survived its partner's normal exit
```

Standard error (actor `a` is reported too, with the reason it inherited):

```text
gogo: actor <pid 2> crashed: divzero in part
gogo: actor <pid 1> crashed: divzero in part
```

A `Normal` exit sends a signal as well, but a non-trapping actor ignores it,
which is why `c` survives. `SpawnLink` links before the child runs, so
there is no window in which the child could die unnoticed; `Spawn` followed
by `Link` has such a window. Linking to an actor that has already exited
delivers a `NoProc` signal at once.

Links (and the monitors of 5.5) work the same on an actor on another
board (4.8). If that actor's whole node becomes unreachable, the signal
carries the reason `actor.NoConnection` (11) instead of the actor's own;
see [20.5](20-distributed.md#205-when-a-node-goes-away).

## 5.4 Trapping exits

An actor that calls `actor.TrapExit(true)` is not taken down by exit
signals. Instead each signal arrives as a message:

```go
type ExitMsg struct {
	From   PID
	Reason Reason
}
```

This is how a supervisor watches its children. `actor.Kill(p, code)` sends
an exit signal on purpose; a trapping actor receives it as an `ExitMsg`
like any other, with one exception: `Kill(p, actor.Killed)` cannot be
trapped and always ends the target.

```go
// TrapExit turns exit signals from linked actors into ExitMsg messages, so
// an actor can react to a partner's death instead of dying with it.
package main

import "gogo/actor"

func flaky(n int) {
	if n%2 == 1 {
		zero := 0
		println(1 / zero)
	}
	actor.Exit(100 + n)
}

func main() {
	actor.TrapExit(true)
	var pids []actor.PID
	for i := 0; i < 3; i++ {
		pids = append(pids, actor.SpawnLink(func() { flaky(i) })) // each iteration has its own i
	}
	for i := 0; i < 3; i++ {
		switch m := actor.Receive().(type) {
		case actor.ExitMsg:
			who := -1
			for j, p := range pids {
				if p == m.From {
					who = j
				}
			}
			println("worker", who, "exited with", m.Reason.Code)
		}
	}

	// Kill with Shutdown is trapped like any other signal; Killed cannot be.
	t := actor.SpawnLink(func() {
		actor.TrapExit(true)
		for {
			switch m := actor.Receive().(type) {
			case actor.ExitMsg:
				println("target trapped code", m.Reason.Code)
			}
		}
	})
	actor.Sleep(10)
	actor.Kill(t, actor.Shutdown)
	actor.Sleep(10)
	actor.Kill(t, actor.Killed)
	switch m := actor.Receive().(type) {
	case actor.ExitMsg:
		println("target gone, code", m.Reason.Code)
	}
}
```

```text
worker 0 exited with 100
worker 1 exited with 5
worker 2 exited with 102
target trapped code 7
target gone, code 1
```

Standard error:

```text
gogo: actor <pid 1> crashed: exit(100)
gogo: actor <pid 2> crashed: divzero in flaky
gogo: actor <pid 3> crashed: exit(102)
gogo: actor <pid 1> crashed: killed
```

Each closure gets its own `i`: as in Go 1.22, every iteration of the loop
has its own loop variable (chapter 3).

## 5.5 Monitors

A **monitor** is a one-way link that never kills anybody. The watcher gets
a message when the watched actor exits, for any reason:

```go
func Monitor(p PID) Ref
func Demonitor(r Ref)

type DownMsg struct {
	Ref    Ref     // the Ref Monitor returned
	From   PID
	Reason Reason
}
```

Use a monitor when you depend on an actor but should not die with it: a
client watching a server, a manager watching jobs. Combined with `Call` it
tells you when a server is gone:

```go
// Monitors watch an actor one way: the watcher gets a DownMsg and is never
// taken down itself. Pair Call with a monitor to notice a dead server early.
package main

import "gogo/actor"

type Work struct{ N int }

func server() {
	for {
		switch r := actor.Receive().(type) {
		case actor.Request:
			switch w := r.Body.(type) {
			case Work:
				actor.Reply(r, 100/w.N) // N == 0 crashes the server
			}
		}
	}
}

func main() {
	s := actor.Spawn(server)
	ref := actor.Monitor(s)

	reply, ok := actor.Call(s, Work{4}, 1000)
	println("reply", reply, ok)

	// This request crashes the server. Call does not return early when the
	// server dies: it waits out its timeout.
	start := actor.Now()
	_, ok = actor.Call(s, Work{0}, 200)
	println("ok:", ok, "waited the full timeout:", actor.Now()-start >= 200)

	// The DownMsg was queued meanwhile.
	switch m := actor.Receive().(type) {
	case actor.DownMsg:
		println("server down:", m.Ref == ref, m.Reason.Code)
	}

	// Monitoring an actor that is already gone gives a DownMsg at once,
	// with reason NoProc.
	actor.Monitor(s)
	switch m := actor.Receive().(type) {
	case actor.DownMsg:
		println("late monitor:", m.Reason.Code == actor.NoProc)
	}
}
```

```text
reply 25 true
ok: false waited the full timeout: true
server down: true 5
late monitor: true
```

Standard error:

```text
gogo: actor <pid 1> crashed: divzero in server
```

Monitoring an actor that does not exist (any more) delivers a `DownMsg`
with `NoProc` straight away, so there is no race between looking up a PID
and monitoring it. `Demonitor` cancels a monitor; no `DownMsg` arrives
afterwards. A monitor of an actor on another board delivers a `DownMsg`
with `actor.NoConnection` when that board, or the server between, goes
away ([20.5](20-distributed.md#205-when-a-node-goes-away)).

> **Good to know.** The `ExitMsg` or `DownMsg` an exit will need is
> reserved when you create the link or monitor, and charged to the actor
> that created it. When an actor exits, the notifications are delivered
> without allocating memory, so they arrive even when memory is short.

## 5.6 Supervisors

A **supervisor** is an actor whose only job is to start a child, watch it,
and restart it when it fails. `gogo/supervisor` provides one, written in the
gogo subset with the pieces from this chapter (it is about 120 lines; read
`lib/supervisor/supervisor.gg` to see exactly what it does).

```go
func Supervise(child func(), spec Spec) actor.PID

type Spec struct {
	Restart     int // supervisor.Permanent or supervisor.Transient
	MaxRestarts int // restarts allowed within one window
	PeriodMs    int // window length in ms; <= 0: never expires
}
```

- `Supervise` starts a supervisor linked to the caller, which starts
  `child` linked to itself.
- **Permanent**: restart the child however it ends, even `Normal`.
- **Transient**: restart only after an abnormal exit; when the child ends
  with `Normal` or `Shutdown`, the supervisor exits `Normal`.
- **Restart intensity**: if the child needs more than `MaxRestarts`
  restarts within `PeriodMs`, something is seriously wrong; the supervisor
  gives up and exits with `Shutdown`. Its own supervisor, if any, then
  decides what to do. The window opens at the first restart and the count
  starts over once `PeriodMs` has passed.
- When the actor that called `Supervise` exits (even normally), the
  supervisor stops the child (asking with `Shutdown`, then after 1 s with
  `Killed`) and exits too. So **`main` must not return while the tree
  should keep running.**

Here is "let it crash" in full. The worker does no error checking at all:
on bad input it just exits. The supervisor brings it back, and the service
keeps answering:

```go
// Let it crash: a worker parses commands and crashes on bad ones. A
// supervisor restarts it, so the service keeps working.
package main

import (
	"gogo/actor"
	"gogo/strconv"
	"gogo/supervisor"
)

type Ready struct{ From actor.PID }

// parse turns "n" into n, and crashes on anything else: no error handling
// at all, the supervisor takes care of failure.
func parse(s string) int {
	n, err := strconv.Atoi(s)
	if err != nil {
		actor.Exit(100) // "bad input"
	}
	return n
}

func worker(boss actor.PID) {
	actor.Send(boss, Ready{actor.Self()})
	for {
		switch r := actor.Receive().(type) {
		case actor.Request:
			switch s := r.Body.(type) {
			case string:
				actor.Reply(r, parse(s)*2)
			}
		}
	}
}

func main() {
	me := actor.Self()
	supervisor.Supervise(func() { worker(me) }, supervisor.Spec{
		Restart:     supervisor.Permanent,
		MaxRestarts: 3,
		PeriodMs:    1000,
	})
	var none, w actor.PID // zero PIDs
	for _, in := range []string{"21", "oops", "50", "x", "4"} {
		// Every new instance says Ready first; take the newest PID.
		for {
			m, ok := actor.ReceiveTimeout(0)
			if !ok {
				break
			}
			switch r := m.(type) {
			case Ready:
				w = r.From
			}
		}
		if w == none {
			switch r := actor.Receive().(type) {
			case Ready:
				w = r.From
			}
		}
		reply, ok := actor.Call(w, in, 100)
		if ok {
			println(in, "->", reply)
		} else {
			println(in, "-> no answer (worker crashed)")
			w = none
		}
	}
	// main returns: the supervisor shuts the worker down and exits too.
}
```

```text
21 -> 42
oops -> no answer (worker crashed)
50 -> 100
x -> no answer (worker crashed)
4 -> 8
```

Standard error:

```text
gogo: actor <pid 2> crashed: exit(100)
gogo: actor <pid 2> crashed: exit(100)
```

The request that caused the crash got no answer (main's `Call` timed out),
but the next request found a fresh worker. In a real program the client
would report "bad input", retry, or move on.

A new child instance has a new PID. That is why this worker announces
itself with `Ready`. A worker that offers a service to many clients would
`actor.Register` a name when it starts instead (section 4.4).

When restarts do not help, the supervisor gives up:

```go
// Restart intensity: a child that keeps crashing makes its supervisor give
// up (exit Shutdown) after MaxRestarts restarts within PeriodMs. The
// supervisor is linked to its caller; trapping exits lets main see it.
package main

import (
	"gogo/actor"
	"gogo/supervisor"
)

func main() {
	actor.TrapExit(true)
	me := actor.Self()
	sup := supervisor.Supervise(func() {
		actor.Send(me, "child started")
		zero := 0
		println(1 / zero)
	}, supervisor.Spec{Restart: supervisor.Permanent, MaxRestarts: 2, PeriodMs: 5000})
	for {
		switch m := actor.Receive().(type) {
		case string:
			println(m)
		case actor.ExitMsg:
			if m.From == sup {
				println("supervisor gave up, code", m.Reason.Code)
				return
			}
		}
	}
}
```

```text
child started
child started
child started
supervisor gave up, code 7
```

Standard error:

```text
gogo: actor <pid 2> crashed: divzero in main.main.func1
gogo: actor <pid 2> crashed: divzero in main.main.func1
gogo: actor <pid 2> crashed: divzero in main.main.func1
```

With `MaxRestarts: 2` the child ran three times (the first start and two
restarts); the third crash was one too many and the supervisor exited with
code 7, `Shutdown`.

### Supervision trees

A supervisor's child can itself start supervisors, giving a tree: a
top-level supervisor restarts a subsystem, whose own supervisor restarts
the workers inside it. If an inner supervisor gives up, it exits, its parent
child (linked to it) exits too, and the outer supervisor restarts the whole
subsystem. `tests/golden/sup_tree` in the gogo tree is a two-level example.

The current supervisor handles **one child**. To supervise several
independent workers, give each its own supervisor; for workers that must
restart together, start them from one child function and link them.

`examples/sticks3_demo` in the gogo tree runs this on a real M5StickS3:
pressing KEY1 makes a worker divide by zero, the supervisor restarts it, and
the demo stops when crashes come too fast.

## 5.7 What gets cleaned up

When an actor exits, for any reason, the VM:

- frees its heap and its mailbox;
- closes every port it owns: GPIO pins, files (committing what was
  written), network sockets, the keyboard and battery ports (chapter 7);
- removes its registered names, its monitors, and the timers that would
  deliver to it (a `SendAfter` it set up for *another* actor still fires);
- sends exit signals to linked actors and `DownMsg` to monitors.

This is why cleanup in gogo does not depend on `defer`: there is nothing
you could forget to release when an actor ends. gogo's `defer` (section
3.4) runs on normal returns only, for actors that live on and must
release something before they continue; a crash skips it, and this
cleanup takes over.
