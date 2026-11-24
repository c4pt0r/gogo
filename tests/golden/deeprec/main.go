package main

import "gogo/actor"

type Done struct{}

func r(n int) int {
	return r(n+1) + 1
}

func main() {
	actor.Spawn(func() {
		println(r(0)) // runs out of memory: oom crashes this actor only
	})
	me := actor.Self()
	actor.Spawn(func() {
		actor.Send(me, Done{})
	})
	switch actor.Receive().(type) {
	case Done:
	}
	println("survived")
}
