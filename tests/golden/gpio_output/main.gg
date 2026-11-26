// Writes to an output pin print "gpio: pin N = V" on stderr.
package main

import (
	"gogo/actor"
	"gogo/port"
)

func main() {
	p, ok := port.Open("gpio", port.GPIO{Pin: 5, Mode: port.Output})
	println(ok)
	actor.Send(p, port.Write{Value: 1})
	actor.Send(p, port.Write{Value: 0})
	port.Close(p)
	println("done")
}
