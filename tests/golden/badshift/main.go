package main

import "gogo/actor"

type Result struct{ N int }

func main() {
	neg := -1
	actor.Spawn(func() {
		println(8 >> neg) // crashes this actor only: badshift
	})
	me := actor.Self()
	actor.Spawn(func() {
		actor.Send(me, Result{7})
	})
	switch m := actor.Receive().(type) {
	case Result:
		println(m.N)
	}
	println("main ok")
}
