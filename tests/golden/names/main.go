package main

import "gogo/actor"

type Boom struct{}

func server() {
	for {
		switch r := actor.Receive().(type) {
		case actor.Request:
			actor.Reply(r, 42)
		case Boom:
			zero := 0
			println(1 / zero)
		}
	}
}

func main() {
	srv := actor.Spawn(func() { server() })
	println(actor.Register("svc", srv))
	// The name is taken.
	other := actor.Spawn(func() { server() })
	println(actor.Register("svc", other))

	p, ok := actor.Whereis("svc")
	println(ok, p == srv)
	reply, ok := actor.Call(p, 1, 100)
	println(reply, ok)

	// An unknown name.
	_, ok = actor.Whereis("nope")
	println(ok)

	// The server crashes; its name goes with it.
	ref := actor.Monitor(srv)
	actor.Send(srv, Boom{})
	switch m := actor.Receive().(type) {
	case actor.DownMsg:
		println("down", m.Ref == ref)
	}
	_, ok = actor.Whereis("svc")
	println(ok)

	// The name is free again.
	println(actor.Register("svc", other))
	p, ok = actor.Whereis("svc")
	println(ok, p == other)

	// Unregister frees it too; a dead pid cannot register.
	actor.Unregister("svc")
	_, ok = actor.Whereis("svc")
	println(ok)
	println(actor.Register("ghost", srv))
	// One actor may hold several names.
	println(actor.Register("a", other), actor.Register("b", other))
}
