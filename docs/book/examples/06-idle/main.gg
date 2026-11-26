// Actors are cheap: an idle actor waiting in Receive costs about 150
// bytes. Spawn a thousand, measure one, then stop them all.
package main

import "gogo/actor"

type Quit struct{}

func idle() {
	for {
		switch actor.Receive().(type) {
		case Quit:
			return
		}
	}
}

func main() {
	var pids []actor.PID
	for i := 0; i < 1000; i++ {
		pids = append(pids, actor.Spawn(idle))
	}
	actor.Sleep(10) // let them all reach Receive
	m := actor.Info(pids[500]).Memory
	println("1000 idle actors; one uses", m, "bytes, under 256:", m <= 256)
	for _, p := range pids {
		actor.Send(p, Quit{})
	}
	println("all asked to quit")
}
