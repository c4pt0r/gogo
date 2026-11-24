package main

import "gogo/actor"

type Done struct{}

func r(n int) int {
	return r(n+1) + 1
}

// main monitors the recursing actor and waits for its down message
// (OutOfMemory), then shows the rest of the VM is unharmed.
func main() {
	deep := actor.Spawn(func() {
		println(r(0)) // runs out of memory: oom crashes this actor only
	})
	actor.Monitor(deep)
	switch m := actor.Receive().(type) {
	case actor.DownMsg:
		println("down", m.Reason.Code)
	}
	me := actor.Self()
	actor.Spawn(func() {
		actor.Send(me, Done{})
	})
	switch actor.Receive().(type) {
	case Done:
	}
	println("survived")
}
