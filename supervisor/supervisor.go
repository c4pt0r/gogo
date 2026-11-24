// Package supervisor is the gogo supervision library: a single-child
// supervisor that restarts its child according to a Spec. It is written in
// the gogo subset and compiled into every image that imports it.
//
// A supervisor and the actor that calls Supervise are linked. When that
// caller exits for any reason, including a normal return, the supervisor
// shuts its child down and exits too, so the caller (main, typically) must
// not return while the tree should keep running.
package supervisor

import "gogo/actor"

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
	return actor.SpawnLink(func() { run(parent, child, spec) })
}

// run is the supervisor actor's body.
func run(parent actor.PID, child func(), spec Spec) {
	actor.TrapExit(true)
	// The caller may have exited before this actor first ran and started
	// trapping; a Normal exit signal then had no effect and the link is
	// gone. Linking again turns that into an ExitMsg (NoProc) from the
	// parent; while the parent lives it is a no-op.
	actor.Link(parent)
	pid := actor.SpawnLink(child)
	restarts := 0
	var windowStart int64
	for {
		switch m := actor.Receive().(type) {
		case actor.ExitMsg:
			if m.From == parent {
				shutdown(pid)
				actor.Exit(actor.Shutdown)
				return
			}
			if m.From != pid {
				continue // a stale or foreign link
			}
			// The child has exited; nothing is left to shut down on
			// any of the exits below.
			if spec.Restart == Transient && !abnormal(m.Reason.Code) {
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
			pid = actor.SpawnLink(child)
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
