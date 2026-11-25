// Drive an output pin: blink an LED three times. On the host a Write
// prints "gpio: pin N = V" on stderr instead of touching hardware.
package main

import (
	"gogo/actor"
	"gogo/port"
)

const ledPin = 5

func main() {
	led, ok := port.Open("gpio", port.GPIO{Pin: ledPin, Mode: port.Output})
	if !ok {
		println("cannot open the LED pin")
		return
	}
	for i := 0; i < 3; i++ {
		actor.Send(led, port.Write{Value: 1})
		actor.Sleep(100)
		actor.Send(led, port.Write{Value: 0})
		actor.Sleep(100)
	}
	port.Close(led)
	println("blinked 3 times")
}
