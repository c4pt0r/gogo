package main

import (
	"gogo/actor"
	"gogo/supervisor"
)

// Ready is sent by a child once it traps exits.
type Ready struct{}

// polite stops when its supervisor asks it to shut down.
func polite(boss actor.PID) {
	actor.TrapExit(true)
	actor.Send(boss, Ready{})
	for {
		switch m := actor.Receive().(type) {
		case actor.ExitMsg:
			if m.Reason.Code == actor.Shutdown {
				println("child got shutdown")
				return
			}
		}
	}
}

// stubborn ignores the shutdown request; after 1000 ms its supervisor
// kills it (the "crashed: killed" report on stderr).
func stubborn(boss actor.PID) {
	actor.TrapExit(true)
	actor.Send(boss, Ready{})
	for {
		actor.Receive()
	}
}

// When main returns, each supervisor linked to it shuts its child down.
func main() {
	me := actor.Self()
	spec := supervisor.Spec{Restart: supervisor.Permanent, MaxRestarts: 3, PeriodMs: 1000}
	supervisor.Supervise(func() { polite(me) }, spec)
	supervisor.Supervise(func() { stubborn(me) }, spec)
	for ready := 0; ready < 2; ready++ {
		actor.Receive()
	}
	println("main returning")
}
