// Time: Now, Sleep, ReceiveTimeout, SendAfter and CancelTimer.
package main

import "gogo/actor"

type Tick struct{ N int }

func main() {
	start := actor.Now()
	actor.Sleep(30)
	println("slept at least 30 ms:", actor.Now()-start >= 30)

	// ReceiveTimeout: wait at most 20 ms. 0 only looks.
	_, ok := actor.ReceiveTimeout(20)
	println("got a message:", ok)

	// SendAfter delivers a message later; timers fire in deadline order.
	me := actor.Self()
	actor.SendAfter(40, me, "late")
	actor.SendAfter(10, me, "early")
	t := actor.SendAfter(20, me, "cancelled")
	println("cancel:", actor.CancelTimer(t))
	println(actor.Receive())
	println(actor.Receive())

	// A periodic tick: re-arm the timer each time it fires.
	actor.SendAfter(10, me, Tick{1})
	last := 0
	for last < 3 {
		switch m := actor.Receive().(type) {
		case Tick:
			println("tick", m.N)
			last = m.N
			if m.N < 3 {
				actor.SendAfter(10, me, Tick{m.N + 1})
			}
		}
	}
	println("done")
}
