// Drives the host port "hp" by posts to "ctl" (Kinds below 1000); the
// port's events (Kinds from 1000) are printed by whoever owns it.
//
//	1: open "hp" (Arg 1)                          prints "open true"
//	2: send the port port.Post{5, "cmd"}
//	3: close it, then send its stale PID a Post   prints "closed"
//	4: hand it to a new actor, which prints its events and returns
//	   on Kind 1999 (its exit closes the port)    prints "handoff true"
//	5: a new actor opens "hp" (Arg 2) and returns at once
//	-1: return
package main

import (
	"gogo/actor"
	"gogo/bytes"
	"gogo/port"
)

func kid() {
	for {
		switch m := actor.Receive().(type) {
		case port.Post:
			println("kid event", m.Kind, m.Data.String())
			if m.Kind == 1999 {
				return
			}
		}
	}
}

func opener() {
	_, ok := port.Open("hp", port.Host{Arg: 2})
	println("child opened", ok)
}

func main() {
	actor.Register("ctl", actor.Self())
	println("ready")
	var p actor.PID
	for {
		switch m := actor.Receive().(type) {
		case port.Post:
			if m.Kind >= 1000 {
				println("event", m.Kind, m.Data.Len())
			} else if m.Kind == -1 {
				println("bye")
				return
			} else if m.Kind == 1 {
				q, ok := port.Open("hp", port.Host{Arg: 1})
				p = q
				println("open", ok)
			} else if m.Kind == 2 {
				actor.Send(p, port.Post{Kind: 5, Data: bytes.FromString("cmd")})
			} else if m.Kind == 3 {
				port.Close(p)
				actor.Send(p, port.Post{Kind: 6})
				println("closed")
			} else if m.Kind == 4 {
				println("handoff", port.Handoff(p, actor.Spawn(kid)))
			} else if m.Kind == 5 {
				actor.Spawn(opener)
			}
		}
	}
}
