package main

import "gogo/actor"

func main() {
	me := actor.Self()
	actor.SendAfter(30, me, "B")
	actor.SendAfter(10, me, "A")
	c := actor.SendAfter(20, me, "C")
	println(actor.CancelTimer(c)) // pending: cancelled
	println(actor.CancelTimer(c)) // already gone

	// Delivered in deadline order, not call order.
	println(actor.Receive())
	println(actor.Receive())

	// The cancelled "C" never arrives.
	m, ok := actor.ReceiveTimeout(60)
	if !ok {
		println("timeout")
	} else {
		println("unexpected", m)
	}
}
