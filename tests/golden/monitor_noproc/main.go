package main

import "gogo/actor"

type Done struct{}

// Monitoring a pid that has already exited queues a NoProc down message
// at once. It relies on the FIFO scheduler: dead finishes exiting (after
// its Send) before main is rescheduled.
func main() {
	me := actor.Self()
	dead := actor.Spawn(func() {
		actor.Send(me, Done{})
	})
	switch actor.Receive().(type) {
	case Done:
	}
	r := actor.Monitor(dead)
	switch m := actor.Receive().(type) {
	case actor.DownMsg:
		println("down", m.Reason.Code, m.Ref == r, m.From == dead)
	}
}
