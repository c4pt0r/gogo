// A port's events go to its owner. port.Handoff gives the port to another
// actor: main opens the button and hands it to a dedicated key handler.
package main

import (
	"gogo/actor"
	"gogo/port"
)

func keyHandler(boss actor.PID) {
	n := 0
	for n < 2 {
		switch e := actor.Receive().(type) {
		case port.Edge:
			n++
			println("handler: edge on pin", e.Pin)
		}
	}
	actor.Send(boss, "handler done")
}

func main() {
	btn, ok := port.Open("gpio", port.GPIO{Pin: 12, Mode: port.InputPullUp, Edge: port.Falling})
	println("open", ok)
	me := actor.Self()
	h := actor.Spawn(func() { keyHandler(me) })
	println("handoff", port.Handoff(btn, h))
	println("handoff again (main is no longer the owner)", port.Handoff(btn, me))
	println(actor.Receive())
}
