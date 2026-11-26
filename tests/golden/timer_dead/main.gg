package main

import "gogo/actor"

type Ready struct{}

// waitDown blocks until the DownMsg carrying ref arrives.
func waitDown(ref actor.Ref) {
	for {
		switch m := actor.Receive().(type) {
		case actor.DownMsg:
			if m.Ref == ref {
				return
			}
		}
	}
}

func main() {
	me := actor.Self()

	// A SendAfter whose target exits before the timer fires.
	d := actor.Spawn(func() {})
	rd := actor.Monitor(d)
	actor.SendAfter(10, d, "late")
	waitDown(rd)

	// An actor killed while it is blocked in ReceiveTimeout: its pending
	// wakeup must not keep the VM alive for a second or touch it later.
	w := actor.Spawn(func() {
		actor.Send(me, Ready{})
		_, _ = actor.ReceiveTimeout(1000)
		println("unreachable")
	})
	rw := actor.Monitor(w)
	actor.Receive() // Ready: w is now blocked
	actor.Kill(w, actor.Killed)
	waitDown(rw)

	// SendAfter to a pid that is already dead still returns a timer.
	t := actor.SendAfter(5, d, "nobody")
	println(actor.CancelTimer(t))
	actor.SendAfter(5, d, "fires into nothing")
	println("fine")
}
