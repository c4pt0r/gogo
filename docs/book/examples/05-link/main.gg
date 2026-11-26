// Links tie actors' fates together: when one exits abnormally, the actors
// linked to it exit too, with the same reason.
package main

import "gogo/actor"

type Hello struct{ Who string }

func part(name string, boss actor.PID, crash bool) {
	actor.Send(boss, Hello{name})
	if crash {
		zero := 0
		println(1 / zero)
	}
	for {
		actor.Receive()
	}
}

func main() {
	me := actor.Self()
	// a and b are linked: SpawnLink inside a links b to a.
	a := actor.Spawn(func() {
		actor.SpawnLink(func() { part("b", me, true) })
		part("a", me, false)
	})
	ref := actor.Monitor(a)
	for i := 0; i < 2; i++ {
		switch m := actor.Receive().(type) {
		case Hello:
			println("started", m.Who)
		}
	}
	// b crashed; a, linked to it and not trapping exits, went down too.
	switch m := actor.Receive().(type) {
	case actor.DownMsg:
		println("a went down with b:", m.Ref == ref, m.Reason.Code == actor.DivZero)
	}

	// A normal exit does not take linked actors down.
	c := actor.Spawn(func() {
		actor.SpawnLink(func() {}) // returns at once: Normal
		actor.Sleep(20)
		actor.Send(me, "c survived its partner's normal exit")
	})
	_ = c
	println(actor.Receive())
}
