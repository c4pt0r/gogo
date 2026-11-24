package main

import "gogo/actor"

func main() {
	// Nobody sends: the wait ends by timeout.
	_, ok := actor.ReceiveTimeout(20)
	if !ok {
		println("timeout")
	}

	// A queued message is returned at once by a zero timeout.
	actor.Send(actor.Self(), 1)
	m, ok := actor.ReceiveTimeout(0)
	if ok {
		println("got", m)
	}

	// A zero timeout on an empty mailbox never blocks.
	_, ok = actor.ReceiveTimeout(0)
	if !ok {
		println("empty")
	}

	// A message arriving during a long wait ends it early...
	me := actor.Self()
	actor.Spawn(func() {
		actor.Send(me, 2)
	})
	m, ok = actor.ReceiveTimeout(5000)
	println("got", m, ok)

	// ...and the cancelled wakeup does not cut the next wait short or
	// leave the actor runnable: this one still times out.
	_, ok = actor.ReceiveTimeout(20)
	if !ok {
		println("timeout")
	}
}
