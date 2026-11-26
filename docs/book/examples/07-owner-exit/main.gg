// A port belongs to the actor that opened it, and closes when that actor
// exits, also when it crashes. So a restarted worker can open the same pin
// again: no leaked handles, no cleanup code.
package main

import (
	"gogo/actor"
	"gogo/port"
)

func worker(boss actor.PID, crash bool) {
	_, ok := port.Open("gpio", port.GPIO{Pin: 5, Mode: port.Output})
	actor.Send(boss, ok)
	if crash {
		zero := 0
		println(1 / zero)
	}
}

func main() {
	me := actor.Self()
	actor.Spawn(func() { worker(me, true) })
	println("first worker opened pin 5:", actor.Receive())
	actor.Sleep(10)
	actor.Spawn(func() { worker(me, false) })
	println("second worker opened pin 5:", actor.Receive())

	// While someone owns it, nobody else can open it.
	_, ok := port.Open("gpio", port.GPIO{Pin: 7, Mode: port.Output})
	_, again := port.Open("gpio", port.GPIO{Pin: 7, Mode: port.Output})
	println("pin 7 first open:", ok, "second open:", again)
}
