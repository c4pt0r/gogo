// Every actor has a memory limit (the platform default, or Options.
// MaxMemory with SpawnOpt). Going over it ends that actor with
// OutOfMemory; nobody else is affected. actor.Info reports usage.
package main

import "gogo/actor"

func grow() {
	var xs []int
	for {
		xs = append(xs, len(xs))
		if len(xs)%1000 == 0 {
			println("  holding", len(xs), "ints, memory", actor.Info(actor.Self()).Memory >= len(xs)*4)
		}
	}
}

func main() {
	me := actor.Self()
	println("main uses", actor.Info(me).Memory, "bytes")
	p := actor.SpawnOpt(func() { grow() }, actor.Options{MaxMemory: 16384})
	actor.Monitor(p)
	switch m := actor.Receive().(type) {
	case actor.DownMsg:
		println("grower exited, OutOfMemory:", m.Reason.Code == actor.OutOfMemory)
	}
	println("main still runs")
}
