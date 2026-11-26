package main

import "gogo/actor"

type Ready struct{}

type Trapped struct{ Code int }

// Kill with Shutdown is trapped (the target reports it); Kill with Killed
// cannot be trapped: the target exits, and main, linked and trapping, gets
// an ExitMsg with code Killed.
func main() {
	actor.TrapExit(true)
	me := actor.Self()
	t := actor.SpawnLink(func() {
		actor.TrapExit(true)
		actor.Send(me, Ready{})
		for {
			switch m := actor.Receive().(type) {
			case actor.ExitMsg:
				actor.Send(me, Trapped{m.Reason.Code})
			}
		}
	})
	switch actor.Receive().(type) {
	case Ready:
	}
	actor.Kill(t, actor.Shutdown)
	switch m := actor.Receive().(type) {
	case Trapped:
		println("trapped", m.Code)
	}
	actor.Kill(t, actor.Killed)
	switch m := actor.Receive().(type) {
	case actor.ExitMsg:
		if m.From == t {
			println("killed", m.Reason.Code)
		}
	}
}
