package main

import "gogo/actor"

func crash() {
	zero := 0
	println(1 / zero)
}

// main traps exits and is linked to A; A is linked to B; B crashes. A does
// not trap, so it exits with B's reason, and main gets an ExitMsg from A.
func main() {
	actor.TrapExit(true)
	a := actor.SpawnLink(func() {
		actor.SpawnLink(func() {
			crash()
		})
		for {
			actor.Receive()
		}
	})
	switch m := actor.Receive().(type) {
	case actor.ExitMsg:
		if m.From == a {
			println("A down:", m.Reason.Code)
		}
	}
}
