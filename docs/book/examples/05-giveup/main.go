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
