package main

import "gogo/actor"

func server() {
	for {
		m := actor.Receive()
		switch r := m.(type) {
		case actor.Request:
			switch b := r.Body.(type) {
			case int:
				actor.Reply(r, b*2)
			}
		}
	}
}

func main() {
	srv := actor.Spawn(func() { server() })

	// A normal message queued before the call is not consumed by it.
	actor.Send(actor.Self(), "kept")

	reply, ok := actor.Call(srv, 21, 100)
	println(reply, ok)

	// A second call gets its own reply.
	reply, ok = actor.Call(srv, 4, 100)
	println(reply, ok)

	// The earlier message is still there.
	m := actor.Receive()
	println(m)
	_, ok = actor.ReceiveTimeout(10)
	println(ok)
}
