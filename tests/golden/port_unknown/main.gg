// Open of a driver the platform does not have fails; Close of the PID a
// failed Open returned is a no-op.
package main

import (
	"gogo/actor"
	"gogo/port"
)

func main() {
	p, ok := port.Open("nosuch", port.GPIO{Pin: 1, Mode: port.Output, Edge: port.None})
	println(ok)
	port.Close(p)
	var q actor.PID
	q, ok = port.Open("nosuch", 5)
	println(ok)
	port.Close(q)
	println("done")
}
