// Count button presses. KEY1 on the M5StickS3 is G11, active low: open it
// with a pull-up and ask for falling edges. Each press arrives as a
// port.Edge message. On the host the presses come from the stdin script
// in the file "input" ("11 0" = pin 11 goes low).
package main

import (
	"gogo/actor"
	"gogo/port"
)

const key1 = 11

func main() {
	_, ok := port.Open("gpio", port.GPIO{Pin: key1, Mode: port.InputPullUp, Edge: port.Falling})
	if !ok {
		println("cannot open KEY1")
		return
	}
	presses := 0
	for presses < 3 {
		switch e := actor.Receive().(type) {
		case port.Edge:
			presses++
			println("press", presses, "pin", e.Pin, "level", e.Value)
		}
	}
	println("three presses, bye")
}
