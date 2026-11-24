package main

import "gogo/actor"

type Done struct{ N int }

func r(n int) int {
	return r(n+1) + 1
}

func main() {
	actor.SpawnOpt(func() {
		println(r(0)) // hits its 8192-byte limit: oom in r
	}, actor.Options{MaxMemory: 8192})
	me := actor.Self()
	// A limit below the initial block is raised to fit it; a huge one is
	// clamped. Both children run.
	actor.SpawnOpt(func() {
		actor.Send(me, Done{1})
	}, actor.Options{MaxMemory: 1})
	actor.SpawnOpt(func() {
		actor.Send(me, Done{2})
	}, actor.Options{MaxMemory: 2000000000})
	for i := 0; i < 2; i++ {
		switch m := actor.Receive().(type) {
		case Done:
			println("done", m.N)
		}
	}
	println("ok")
}
