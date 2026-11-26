package main

import (
	"gogo/actor"
	"gogo/supervisor"
)

// Hello is what every new worker instance sends main first.
type Hello struct{ From actor.PID }

// Crash tells a worker to crash; Ping asks it to answer with Pong.
type Crash struct{}

type Ping struct{ From actor.PID }

type Pong struct{}

func worker(boss actor.PID) {
	actor.Send(boss, Hello{actor.Self()})
	for {
		switch m := actor.Receive().(type) {
		case Crash:
			zero := 0
			println(1 / zero)
		case Ping:
			actor.Send(m.From, Pong{})
		}
	}
}

// A Permanent worker crashes twice on command; the supervisor restarts it
// each time, so main meets three distinct instances.
func main() {
	me := actor.Self()
	supervisor.Supervise(func() { worker(me) }, supervisor.Spec{
		Restart:     supervisor.Permanent,
		MaxRestarts: 5,
		PeriodMs:    1000,
	})
	first := true
	var prev actor.PID
	var cur actor.PID
	for n := 1; n <= 3; n++ {
		switch m := actor.Receive().(type) {
		case Hello:
			cur = m.From
		}
		if first || cur != prev {
			println("started", n)
		} else {
			println("same instance", n)
		}
		first = false
		prev = cur
		if n < 3 {
			actor.Send(cur, Crash{})
		}
	}
	// The third instance works; returning then shuts the tree down.
	actor.Send(cur, Ping{me})
	switch actor.Receive().(type) {
	case Pong:
		println("third instance answers")
	}
}
