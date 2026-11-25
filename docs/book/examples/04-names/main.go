// Registered names: find a service by name instead of passing its PID.
package main

import "gogo/actor"

type Log struct{ Line string }

type Flush struct{ From actor.PID }

func logger() {
	n := 0
	for {
		switch m := actor.Receive().(type) {
		case Log:
			n++
			println("log", n, m.Line)
		case Flush:
			actor.Send(m.From, n)
		}
	}
}

// logf sends to the "log" service if it runs.
func logf(line string) {
	if p, ok := actor.Whereis("log"); ok {
		actor.Send(p, Log{line})
	}
}

func main() {
	logf("lost: nobody is registered yet")
	l := actor.Spawn(logger)
	println("register", actor.Register("log", l))
	println("again", actor.Register("log", actor.Self())) // the name is taken

	done := actor.Self()
	for i := 0; i < 2; i++ {
		actor.Spawn(func() {
			logf("from a worker")
			actor.Send(done, true)
		})
		actor.Receive()
	}
	logf("from main")
	actor.Send(l, Flush{actor.Self()})
	println("lines logged:", actor.Receive())

	actor.Unregister("log")
	_, ok := actor.Whereis("log")
	println("after unregister:", ok)
}
