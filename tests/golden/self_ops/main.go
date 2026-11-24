package main

import "gogo/actor"

// Linking or unlinking oneself does nothing; Kill of oneself while trapping
// delivers an ExitMsg from oneself.
func main() {
	actor.Link(actor.Self())
	actor.Unlink(actor.Self())
	println("self link ok")
	actor.TrapExit(true)
	actor.Kill(actor.Self(), 120)
	switch m := actor.Receive().(type) {
	case actor.ExitMsg:
		if m.From == actor.Self() {
			println("self kill", m.Reason.Code)
		}
	}
}
