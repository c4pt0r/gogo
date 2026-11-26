package main

import "gogo/actor"

type Result struct{ N int }

func main() {
	zero := 0
	actor.Spawn(func() {
		println(1 / zero) // crashes this actor only: divzero
	})
	me := actor.Self()
	actor.Spawn(func() {
		actor.Send(me, Result{42})
	})
	switch m := actor.Receive().(type) {
	case Result:
		println(m.N)
	}
	println("main ok")
}
