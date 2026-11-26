package main

import "gogo/actor"

type Ping struct{ From actor.PID }

type Pong struct{}

type Done struct{}

func crash() {
	zero := 0
	println(1 / zero)
}

// A trapping worker survives the crash of an actor linked to it and keeps
// working: it still answers a round-trip with main afterwards.
func main() {
	me := actor.Self()
	actor.Spawn(func() {
		actor.TrapExit(true)
		actor.SpawnLink(func() {
			crash()
		})
		switch m := actor.Receive().(type) {
		case actor.ExitMsg:
			println("worker got exit", m.Reason.Code)
		}
		actor.Send(me, Ping{actor.Self()})
		switch actor.Receive().(type) {
		case Pong:
			println("worker alive")
		}
		actor.Send(me, Done{})
	})
	switch m := actor.Receive().(type) {
	case Ping:
		actor.Send(m.From, Pong{})
	}
	switch actor.Receive().(type) {
	case Done:
		println("done")
	}
}
