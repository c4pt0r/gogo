package main

import "gogo/actor"

type Ping struct{ From actor.PID }

type Pong struct{}

func main() {
	var last actor.PID
	for i := 0; i < 1000; i++ {
		last = actor.Spawn(func() {
			for {
				switch m := actor.Receive().(type) {
				case Ping:
					actor.Send(m.From, Pong{})
				}
			}
		})
	}
	// One round-trip with the last actor: it has reached Receive.
	actor.Send(last, Ping{actor.Self()})
	switch actor.Receive().(type) {
	case Pong:
	}
	println(actor.Info(last).Memory)
}
