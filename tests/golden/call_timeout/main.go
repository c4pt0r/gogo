package main

import "gogo/actor"

func main() {
	me := actor.Self()
	// The server takes the request but only answers after the call gave up.
	srv := actor.Spawn(func() {
		m := actor.Receive()
		switch r := m.(type) {
		case actor.Request:
			actor.Sleep(80)
			actor.Reply(r, 1)
			actor.Send(me, "done")
		}
	})

	reply, ok := actor.Call(srv, 5, 20)
	println(reply, ok)

	// The late reply is invisible to Receive/ReceiveTimeout; only "done" shows.
	m, ok := actor.ReceiveTimeout(500)
	println(m, ok)
	_, ok = actor.ReceiveTimeout(50)
	if !ok {
		println("timeout")
	}
}
