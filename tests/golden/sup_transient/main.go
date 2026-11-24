package main

import (
	"gogo/actor"
	"gogo/supervisor"
)

// Hello is what every new child instance sends main first.
type Hello struct{ From actor.PID }

// Commands main sends the child.
type Crash struct{}

type Finish struct{}

type Quit struct{}

func child(boss actor.PID) {
	actor.Send(boss, Hello{actor.Self()})
	switch actor.Receive().(type) {
	case Crash:
		zero := 0
		println(1 / zero)
	case Finish:
		return
	case Quit:
		actor.Exit(actor.Shutdown)
	}
}

// waitHello returns the PID of the next child instance.
func waitHello() actor.PID {
	for {
		switch m := actor.Receive().(type) {
		case Hello:
			return m.From
		}
	}
}

// waitSupExit prints the exit code of the supervisor sup.
func waitSupExit(sup actor.PID) {
	for {
		switch m := actor.Receive().(type) {
		case actor.ExitMsg:
			if m.From == sup {
				println("sup exit", m.Reason.Code)
				return
			}
		}
	}
}

func transient(me actor.PID) actor.PID {
	return supervisor.Supervise(func() { child(me) }, supervisor.Spec{
		Restart:     supervisor.Transient,
		MaxRestarts: 3,
		PeriodMs:    1000,
	})
}

// A Transient child is restarted only after an abnormal exit; when it
// finishes with Normal or Shutdown, its supervisor exits Normal.
func main() {
	actor.TrapExit(true)
	me := actor.Self()

	println("run 1: normal exit")
	sup := transient(me)
	actor.Send(waitHello(), Finish{})
	println("started 1")
	waitSupExit(sup)

	println("run 2: crash, then normal exit")
	sup = transient(me)
	actor.Send(waitHello(), Crash{})
	println("started 1")
	actor.Send(waitHello(), Finish{})
	println("started 2")
	waitSupExit(sup)

	println("run 3: shutdown exit")
	sup = transient(me)
	actor.Send(waitHello(), Quit{})
	println("started 1")
	waitSupExit(sup)
}
