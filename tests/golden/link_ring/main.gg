package main

import "gogo/actor"

type Next struct{ P actor.PID }

type Ready struct{}

type Go struct{}

// member links to the next member of the ring, reports ready, then
// crashes when told to.
func member(main actor.PID) {
	switch m := actor.Receive().(type) {
	case Next:
		actor.Link(m.P)
	}
	actor.Send(main, Ready{})
	switch actor.Receive().(type) {
	case Go:
		zero := 0
		println(1 / zero)
	}
}

// A-B-C-A linked ring, none trapping; main traps and links A only. C
// crashes, the crash cascades around the ring, and main hears of it once.
func main() {
	actor.TrapExit(true)
	me := actor.Self()
	a := actor.Spawn(func() { member(me) })
	b := actor.Spawn(func() { member(me) })
	c := actor.Spawn(func() { member(me) })
	actor.Send(a, Next{b})
	actor.Send(b, Next{c})
	actor.Send(c, Next{a})
	for i := 0; i < 3; i++ {
		switch actor.Receive().(type) {
		case Ready:
		}
	}
	actor.Link(a)
	actor.Send(c, Go{})
	switch m := actor.Receive().(type) {
	case actor.ExitMsg:
		if m.From == a && m.Reason.Code == actor.DivZero {
			println("ring down")
		}
	}
	// Nothing else arrives: a second message would be printed.
	actor.Send(me, Ready{})
	switch m := actor.Receive().(type) {
	case actor.ExitMsg:
		println("unexpected exit from", m.From)
	case Ready:
	}
}
