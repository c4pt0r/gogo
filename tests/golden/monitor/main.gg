package main

import "gogo/actor"

type Go struct{}

type Done struct{}

func crash() {
	zero := 0
	println(1 / zero)
}

// waitDown blocks for the DownMsg of ref and prints its reason code.
func waitDown(ref actor.Ref, who actor.PID) {
	switch m := actor.Receive().(type) {
	case actor.DownMsg:
		println("down", m.Reason.Code, m.Ref == ref, m.From == who)
	}
}

func main() {
	// A worker that exits normally: its down message carries Normal.
	w1 := actor.Spawn(func() {
		actor.Receive()
	})
	r1 := actor.Monitor(w1)
	actor.Send(w1, Go{})
	waitDown(r1, w1)

	// A worker that crashes: down carries DivZero, and main (a plain
	// monitor, not a link) survives.
	w2 := actor.Spawn(func() {
		actor.Receive()
		crash()
	})
	r2 := actor.Monitor(w2)
	actor.Send(w2, Go{})
	waitDown(r2, w2)

	// Demonitor before the worker exits: no message ever arrives. The
	// round-trip through a later actor shows the mailbox holds only Done.
	w3 := actor.Spawn(func() {
		actor.Receive()
	})
	r3 := actor.Monitor(w3)
	actor.Demonitor(r3)
	actor.Demonitor(r3) // already gone: does nothing
	actor.Send(w3, Go{})
	me := actor.Self()
	actor.Spawn(func() {
		actor.Send(me, Done{})
	})
	switch actor.Receive().(type) {
	case Done:
		println("no down")
	case actor.DownMsg:
		println("unexpected down")
	}

	// Monitoring oneself returns a ref and never delivers.
	rs := actor.Monitor(me)
	println(rs == rs)
	actor.Send(me, Done{})
	switch actor.Receive().(type) {
	case Done:
		println("self: no down")
	case actor.DownMsg:
		println("unexpected down")
	}
	actor.Demonitor(rs)
}
