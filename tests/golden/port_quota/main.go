// A port's events obey the owner's mailbox quota: an owner that never
// receives overflows its mailbox and exits, and main (monitoring) lives on.
package main

import "gogo/actor"
import "gogo/port"

func main() {
	owner := actor.SpawnOpt(func() {
		_, ok := port.Open("gpio", port.GPIO{Pin: 11, Mode: port.InputPullUp, Edge: port.Both})
		if !ok {
			println("open failed")
		}
		actor.Sleep(1000000)
	}, actor.Options{MaxMemory: 1024})
	actor.Monitor(owner)
	switch actor.Receive().(type) {
	case actor.DownMsg:
		println("flooded")
	}
}
