package main

import "gogo/actor"

type Ping struct {
	From actor.PID
	N    int
}

type Pong struct{ N int }

func main() {
	pong := actor.Spawn(func() {
		for {
			switch m := actor.Receive().(type) {
			case Ping:
				actor.Send(m.From, Pong{m.N})
			}
		}
	})
	me := actor.Self()
	for i := 1; i <= 3; i++ {
		println("ping", i)
		actor.Send(pong, Ping{me, i})
		switch m := actor.Receive().(type) {
		case Pong:
			println("pong", m.N)
		}
	}
	println("done")
}
