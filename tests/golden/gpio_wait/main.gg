// The VM waits for input while an input port is open and exits once stdin
// reaches EOF, even though main is still blocked in Receive.
package main

import (
	"gogo/actor"
	"gogo/port"
)

func main() {
	_, ok := port.Open("gpio", port.GPIO{Pin: 11, Mode: port.InputPullUp, Edge: port.Both})
	println(ok)
	for {
		switch e := actor.Receive().(type) {
		case port.Edge:
			println("edge", e.Pin, e.Value)
		}
	}
}
