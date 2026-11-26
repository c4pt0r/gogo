// A supervised worker owns pin 11 and crashes on its first edge. Its exit
// closes the port, so the restarted worker can open pin 11 again and gets
// the next edge.
package main

import (
	"gogo/actor"
	"gogo/port"
	"gogo/supervisor"
)

type Hello struct {
	From actor.PID
	Ok   bool
}

type Num struct{ N int }

type Done struct{}

func worker(boss actor.PID) {
	_, ok := port.Open("gpio", port.GPIO{Pin: 11, Mode: port.InputPullUp, Edge: port.Falling})
	actor.Send(boss, Hello{actor.Self(), ok})
	n := 0
	switch m := actor.Receive().(type) {
	case Num:
		n = m.N
	}
	switch actor.Receive().(type) {
	case port.Edge:
		if n == 1 {
			zero := 0
			println(1 / zero)
		}
		println("worker", n, "got edge")
		actor.Send(boss, Done{})
	}
	actor.Receive()
}

func main() {
	me := actor.Self()
	supervisor.Supervise(func() { worker(me) }, supervisor.Spec{
		Restart:     supervisor.Permanent,
		MaxRestarts: 5,
		PeriodMs:    1000,
	})
	n := 0
	for {
		switch m := actor.Receive().(type) {
		case Hello:
			n++
			if n == 1 {
				println("opened", m.Ok)
			} else {
				println("reopened", m.Ok)
			}
			actor.Send(m.From, Num{n})
		case Done:
			println("done")
			return
		}
	}
}
