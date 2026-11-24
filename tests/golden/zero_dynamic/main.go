package main

import "gogo/actor"

// Zero PIDs and Refs keep their dynamic type in `any`, compare equal to
// each other and to no live value, and copy through messages unchanged.

type Holder struct {
	P actor.PID
	R actor.Ref
}

func classify(x any) {
	switch x.(type) {
	case actor.PID:
		println("PID")
	case actor.Ref:
		println("Ref")
	default:
		println("other")
	}
}

func main() {
	var p actor.PID
	var r actor.Ref
	var x any
	classify(p)
	classify(r)
	classify(x)
	classify(actor.Self())
	classify(actor.MakeRef())

	var p2 actor.PID
	var r2 actor.Ref
	println(p == p2, p != p2, p == actor.Self())
	println(r == r2, r != r2, r == actor.MakeRef())
	println(p, r)

	h := Holder{}
	classify(h.P)
	classify(h.R)
	println(h.P == p, h.R == r)

	// Sending to the zero PID is dropped; the zero Ref cancels nothing.
	actor.Send(p, "lost")
	println(actor.CancelTimer(r))
	actor.Demonitor(r)

	// A failed Whereis yields the zero PID.
	q, ok := actor.Whereis("nobody")
	println(ok, q == p)
	classify(q)

	// Zero values travel through a message unchanged.
	actor.Send(actor.Self(), Holder{})
	switch m := actor.Receive().(type) {
	case Holder:
		println(m.P == p, m.R == r)
		classify(m.P)
		classify(m.R)
	}
	var anyp any = p
	actor.Send(actor.Self(), anyp)
	classify(actor.Receive())

	// Monitoring the zero PID behaves as monitoring a dead actor.
	ref := actor.Monitor(p)
	switch m := actor.Receive().(type) {
	case actor.DownMsg:
		println(m.Ref == ref, m.From == p, m.Reason.Code == actor.NoProc)
	}
	println("ok")
}
