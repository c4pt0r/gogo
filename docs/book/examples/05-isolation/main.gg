// A crash ends only the actor that crashed. Its neighbours keep running.
package main

import "gogo/actor"

func divide(a, b int) int { return a / b }

func main() {
	me := actor.Self()
	for _, d := range []int{2, 0, 5} {
		actor.Spawn(func() {
			actor.Send(me, divide(100, d)) // d == 0 crashes this actor only
		})
	}
	// Two answers arrive; the third worker crashed instead of answering.
	for i := 0; i < 2; i++ {
		println("answer", actor.Receive())
	}
	_, ok := actor.ReceiveTimeout(50)
	println("third answer:", ok)
	println("main is fine")
}
