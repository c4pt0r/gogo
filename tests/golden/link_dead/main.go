package main

import "gogo/actor"

type Done struct{}

// Linking a dead pid is a NoProc exit signal: an ExitMsg when trapping,
// otherwise the linker exits with noproc.
func main() {
	me := actor.Self()
	dead := actor.Spawn(func() {
		actor.Send(me, Done{})
	})
	switch actor.Receive().(type) {
	case Done:
	}
	actor.TrapExit(true)
	actor.Link(dead)
	switch m := actor.Receive().(type) {
	case actor.ExitMsg:
		if m.From == dead {
			println("noproc", m.Reason.Code)
		}
	}
	// A non-trapping linker exits; main, linked and trapping, is told.
	w := actor.SpawnLink(func() {
		actor.Link(dead)
		println("not reached")
	})
	switch m := actor.Receive().(type) {
	case actor.ExitMsg:
		if m.From == w {
			println("linker down", m.Reason.Code)
		}
	}
	// Unlinking a pid that is not linked does nothing.
	actor.Unlink(dead)
	actor.TrapExit(false)
	println("ok")
}
