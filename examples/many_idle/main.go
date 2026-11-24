// many_idle: 1000 actors blocked in Receive fit in the VM's memory cap.
// Spawns them, prints the count and the sum of their actor.Info memory, then
// returns; the VM exiting reclaims them all.
//
//	gogo build examples/many_idle -o many_idle.gogo && gogo flash many_idle.gogo
package main

import "gogo/actor"

func idle() {
	actor.Receive()
}

func main() {
	const n = 1000
	total := 0
	for i := 0; i < n; i++ {
		p := actor.Spawn(idle)
		total += actor.Info(p).Memory
	}
	println("spawned", n)
	println("idle memory total:", total, "B")
}
