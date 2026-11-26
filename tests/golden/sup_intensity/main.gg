package main

import (
	"gogo/actor"
	"gogo/supervisor"
)

// Started is what every instance of the child sends main before crashing.
type Started struct{}

func crashy(boss actor.PID) {
	actor.Send(boss, Started{})
	zero := 0
	println(1 / zero)
}

// count counts Started messages until sup exits, then prints the count and
// the supervisor's exit code.
func count(sup actor.PID) {
	starts := 0
	for {
		switch m := actor.Receive().(type) {
		case Started:
			starts++
		case actor.ExitMsg:
			if m.From == sup {
				println("starts", starts)
				println("supervisor down", m.Reason.Code)
				return
			}
		}
	}
}

// The child crashes as soon as it starts. MaxRestarts 2 allows two restarts
// in the window; the third crash exceeds it, so the supervisor gives up and
// exits with Shutdown, which main (linked to it by Supervise) traps.
// PeriodMs 0 means a window that never expires: the same limit applies.
func main() {
	actor.TrapExit(true)
	me := actor.Self()
	count(supervisor.Supervise(func() { crashy(me) }, supervisor.Spec{
		Restart:     supervisor.Permanent,
		MaxRestarts: 2,
		PeriodMs:    1000,
	}))
	count(supervisor.Supervise(func() { crashy(me) }, supervisor.Spec{
		Restart:     supervisor.Permanent,
		MaxRestarts: 2,
		PeriodMs:    0,
	}))
}
