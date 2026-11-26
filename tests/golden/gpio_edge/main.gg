// Edge filtering of a simulated input: pin 11 (pull-up, starts high) reports
// only falling edges, pin 12 (starts low) only rising ones. stdin lines are
// consumed one per idle scheduler pass, so the output depends only on
// message causality.
package main

import (
	"gogo/actor"
	"gogo/port"
)

func main() {
	p11, ok1 := port.Open("gpio", port.GPIO{Pin: 11, Mode: port.InputPullUp, Edge: port.Falling})
	p12, ok2 := port.Open("gpio", port.GPIO{Pin: 12, Mode: port.Input, Edge: port.Rising})
	println(ok1, ok2)
	for n := 0; n < 3; n++ {
		switch e := actor.Receive().(type) {
		case port.Edge:
			println("edge", e.Pin, e.Value)
		}
	}
	port.Close(p11)
	port.Close(p12)
	println("done")
}
