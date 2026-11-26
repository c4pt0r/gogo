package main

import (
	"gogo/actor"
	"gogo/supervisor"
)

// Sup carries a supervisor's PID to main.
type Sup struct{ PID actor.PID }

func idle() {
	for {
		actor.Receive()
	}
}

// owner starts a supervisor and returns at once, before the supervisor has
// run and started trapping exits. The supervisor must still notice that its
// caller is gone and shut its tree down instead of supervising forever.
func owner(boss actor.PID) {
	sup := supervisor.Supervise(func() { idle() }, supervisor.Spec{
		Restart:     supervisor.Permanent,
		MaxRestarts: 3,
		PeriodMs:    1000,
	})
	actor.Send(boss, Sup{sup})
}

func main() {
	me := actor.Self()
	actor.Spawn(func() { owner(me) })
	var sup actor.PID
	switch m := actor.Receive().(type) {
	case Sup:
		sup = m.PID
	}
	actor.Monitor(sup)
	m, ok := actor.ReceiveTimeout(5000)
	switch m.(type) {
	case actor.DownMsg:
		println("tree down")
	}
	if !ok {
		println("tree still up")
	}
}
