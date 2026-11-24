package main

import "gogo/actor"

func main() {
	t0 := actor.Now()
	me := actor.Self()
	// Sent while main sleeps: Sleep ignores it and it stays queued.
	actor.Spawn(func() {
		actor.Send(me, "hi")
	})
	actor.Sleep(30)
	println(actor.Now()-t0 >= 30)
	m, ok := actor.ReceiveTimeout(0)
	println(m, ok)

	var v any = actor.Now()
	switch v.(type) {
	case int64:
		println("int64")
	case int:
		println("int")
	}
}
