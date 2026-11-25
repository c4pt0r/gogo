// A spawned closure gets copies of what it captures: the new actor shares
// no memory with its parent.
package main

import "gogo/actor"

func main() {
	me := actor.Self()
	data := []int{1, 2, 3}
	actor.Spawn(func() {
		data[0] = 100 // changes the child's copy only
		actor.Send(me, data[0])
	})
	println("child saw", actor.Receive())
	println("parent still has", data[0])

	// Each actor has its own PID; Self tells an actor its own.
	child := actor.Spawn(func() {
		actor.Send(me, actor.Self())
	})
	switch p := actor.Receive().(type) {
	case actor.PID:
		println("same pid:", p == child, p == me)
	}
}
