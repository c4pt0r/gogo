package main

import "gogo/actor"

type Done struct{}

func main() {
	actor.Spawn(func() {
		actor.Exit(150) // reported as exit(150)
		println("not reached")
	})
	me := actor.Self()
	actor.Spawn(func() {
		actor.Send(me, Done{})
	})
	switch actor.Receive().(type) {
	case Done:
	}
	println("ok")
}
