// Package supervisor is the gogo supervision library: a single-child
// supervisor that restarts its child according to a Spec. It is written in
// the gogo subset and compiled into every image that imports it.
//
// A supervisor and the actor that calls Supervise are linked. When that
// caller exits for any reason, including a normal return, the supervisor
// shuts its child down and exits too, so the caller (main, typically) must
// not return while the tree should keep running.
package supervisor

import (
	"gogo/actor"
	"gogo/code"
)

// Restart strategies.
const (
	Permanent = iota // restart the child however it exits
	Transient        // restart the child only after an abnormal exit
)

// Spec configures a supervisor.
type Spec struct {
	Restart     int // Permanent or Transient
	MaxRestarts int // restarts allowed within one window
	PeriodMs    int // window length in milliseconds; <= 0: never expires
}

// shutdownWaitMs is how long a child may take to stop after being asked to
// shut down before it is killed.
const shutdownWaitMs = 1000

// Supervise starts a supervisor actor, linked to the caller, that starts
// child (linked to the supervisor) and restarts it according to spec.
//
// Restart intensity uses a fixed window: the window opens at the first
// restart and lasts PeriodMs; when it has passed, the count starts over.
// With PeriodMs <= 0 the window never expires, so MaxRestarts bounds the
// supervisor's whole lifetime.
// A restart beyond MaxRestarts within the window makes the supervisor give
// up and exit with Shutdown. A Transient child that exits Normal or
// Shutdown is not restarted and the supervisor exits Normal.
func Supervise(child func(), spec Spec) actor.PID {
	parent := actor.Self()
	return actor.SpawnLink(func() { run(parent, code.Module{}, "", 0, child, spec) })
}

// Upgrade is the message that hot-swaps the code of a SuperviseModule
// supervisor. Send it to the PID SuperviseModule returned, either with
// actor.Send (fire and forget) or with actor.Call, whose reply is an int, a code.Err:
// int(code.OK), or why nothing changed.
//
// The supervisor holds Module, stops the child, starts it again (same entry
// and argument) from Module and releases the old module. If that fails the
// supervisor keeps the old module and its hold, restarts the child from it
// and carries on; the Err is Hold's or Spawn's (TooMany, NotFound for a
// module without the entry or already unloaded), or BadArg for the module it
// already runs or for a supervisor of a plain function (Supervise). An
// upgrade is not a restart: it does not count against MaxRestarts. Messages
// sent to the old child between its stop and the new start are lost. Any
// actor can send Upgrade; there is no ownership check in this version.
type Upgrade struct{ Module code.Module }

// SuperviseModule is Supervise for a child that lives in a loaded module:
// the supervisor holds m and starts entry of m with arg (code.SpawnLink),
// then restarts it exactly as Supervise would, from the module.
//
// SuperviseModule returns once the supervisor holds m and has started the
// child, so the caller may Release m (or exit) right after it returns. If
// that cannot be done (m is not loaded, has no such entry, or has 4 holders
// already) the supervisor exits Normal at once and SuperviseModule returns
// the zero PID (which is never alive); the caller has to check for it.
func SuperviseModule(m code.Module, entry string, arg any, spec Spec) actor.PID {
	parent := actor.Self()
	sup := actor.SpawnLink(func() { run(parent, m, entry, arg, func() {}, spec) })
	var none actor.PID
	r, ok := actor.Call(sup, 0, helloWaitMs)
	if !ok {
		return none
	}
	switch e := r.(type) {
	case int:
		if e != int(code.OK) {
			return none
		}
	}
	return sup
}

// helloWaitMs is how long SuperviseModule waits for its supervisor to
// start.
const helloWaitMs = 1000

// start starts the child linked to the caller: entry of m when m is a
// module (m.H != 0), else the function child.
func start(m code.Module, entry string, arg any, child func()) (actor.PID, code.Err) {
	if m.H == 0 {
		return actor.SpawnLink(child), code.OK
	}
	p, e := code.SpawnLink(m, entry, arg)
	return p, e
}

// hello answers SuperviseModule's Call with e. The Call is the first message
// the supervisor gets; anything before it is dropped.
func hello(e code.Err) {
	for {
		switch r := actor.Receive().(type) {
		case actor.Request:
			actor.Reply(r, int(e))
			return
		}
	}
}

// swap is the Upgrade: it moves the child (pid, running from m) to module
// nm. It returns the module the child now runs from, its pid and the result.
// If the old child cannot be restarted either, the supervisor exits.
func swap(m code.Module, nm code.Module, entry string, arg any, child func(), pid actor.PID) (code.Module, actor.PID, code.Err) {
	if m.H == 0 || nm.H == m.H {
		return m, pid, code.BadArg
	}
	if e := code.Hold(nm); e != code.OK {
		return m, pid, e
	}
	shutdown(pid)
	p, e := start(nm, entry, arg, child)
	if e == code.OK {
		code.Release(m)
		return nm, p, code.OK
	}
	code.Release(nm)
	p, e2 := start(m, entry, arg, child)
	if e2 != code.OK {
		actor.Exit(actor.Shutdown)
	}
	return m, p, e
}

// run is the supervisor actor's body.
func run(parent actor.PID, m code.Module, entry string, arg any, child func(), spec Spec) {
	actor.TrapExit(true)
	// The caller may have exited before this actor first ran and started
	// trapping; a Normal exit signal then had no effect and the link is
	// gone. Linking again turns that into an ExitMsg (NoProc) from the
	// parent; while the parent lives it is a no-op.
	actor.Link(parent)
	var pid actor.PID
	e := code.OK
	if m.H != 0 {
		// A module supervisor holds m before it tells SuperviseModule it
		// is up, so the caller may Release m as soon as that returns.
		e = code.Hold(m)
		if e == code.OK {
			pid, e = start(m, entry, arg, child)
		}
		hello(e)
		if e != code.OK {
			actor.Exit(actor.Normal)
			return
		}
	} else {
		pid, e = start(m, entry, arg, child)
	}
	restarts := 0
	var windowStart int64
	for {
		switch x := actor.Receive().(type) {
		case actor.ExitMsg:
			if x.From == parent {
				shutdown(pid)
				actor.Exit(actor.Shutdown)
				return
			}
			if x.From != pid {
				continue // a stale or foreign link
			}
			// The child has exited; nothing is left to shut down on
			// any of the exits below.
			if spec.Restart == Transient && !abnormal(x.Reason.Code) {
				actor.Exit(actor.Normal)
				return
			}
			now := actor.Now()
			if restarts == 0 || windowExpired(now, windowStart, spec.PeriodMs) {
				windowStart = now
				restarts = 0
			}
			restarts++
			if restarts > spec.MaxRestarts {
				actor.Exit(actor.Shutdown)
				return
			}
			pid, e = start(m, entry, arg, child)
			if e != code.OK {
				actor.Exit(actor.Shutdown)
				return
			}
		case Upgrade:
			m, pid, e = swap(m, x.Module, entry, arg, child, pid)
			actor.Link(parent) // shutdown may have dropped its exit
		case actor.Request:
			switch u := x.Body.(type) {
			case Upgrade:
				m, pid, e = swap(m, u.Module, entry, arg, child, pid)
				actor.Link(parent)
				actor.Reply(x, int(e))
			}
		default:
			// Not for us: drop it so the mailbox cannot fill up.
		}
	}
}

// windowExpired reports whether a restart window opened at start has
// passed at now. A window of periodMs <= 0 never expires.
func windowExpired(now int64, start int64, periodMs int) bool {
	return periodMs > 0 && now-start >= int64(periodMs)
}

// abnormal reports whether a child exit with this reason code counts as a
// failure for a Transient child.
func abnormal(code int) bool {
	return code != actor.Normal && code != actor.Shutdown
}

// shutdown stops the child: it asks with Shutdown, waits up to
// shutdownWaitMs for the child's ExitMsg, then kills it outright.
func shutdown(child actor.PID) {
	actor.Kill(child, actor.Shutdown)
	deadline := actor.Now() + shutdownWaitMs
	for {
		left := int(deadline - actor.Now())
		if left <= 0 {
			break
		}
		msg, ok := actor.ReceiveTimeout(left)
		if !ok {
			break
		}
		switch m := msg.(type) {
		case actor.ExitMsg:
			if m.From == child {
				return
			}
		}
	}
	actor.Kill(child, actor.Killed)
}
