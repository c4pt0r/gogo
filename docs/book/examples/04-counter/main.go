// An actor that owns state: a counter. Other actors change it and read it
// only by sending messages.
package main

import "gogo/actor"

type Add struct{ N int }

type Get struct{ From actor.PID }

type Value struct{ N int }

type Stop struct{}

func counter() {
	total := 0
	for {
		switch m := actor.Receive().(type) {
		case Add:
			total += m.N
		case Get:
			actor.Send(m.From, Value{total})
		case Stop:
			println("counter stopping at", total)
			return // returning from the entry function ends the actor
		default:
			println("counter: unknown message dropped")
		}
	}
}

func main() {
	c := actor.Spawn(counter)
	for i := 1; i <= 10; i++ {
		actor.Send(c, Add{i})
	}
	actor.Send(c, "hello?") // any value can be sent; the counter drops it
	actor.Send(c, Get{actor.Self()})
	switch v := actor.Receive().(type) {
	case Value:
		println("total", v.N)
	}
	actor.Send(c, Stop{})
}
