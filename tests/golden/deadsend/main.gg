package main

import "gogo/actor"

type Done struct{}

type Hello struct{ N int }

func main() {
	gone := actor.Spawn(func() {})
	me := actor.Self()
	// gone was queued before the helper, so it has exited by the time
	// the helper's reply arrives.
	actor.Spawn(func() {
		actor.Send(me, Done{})
	})
	switch actor.Receive().(type) {
	case Done:
	}
	actor.Send(gone, Hello{1}) // dropped silently
	println("still alive")
}
