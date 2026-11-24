package main

import "gogo/actor"

// Job pairs a name with the code an actor runs.
type Job struct {
	Name string
	Run  func()
}

// second is a top-level function used as a func value; with no captures it
// finds main through its registered name.
func second() {
	p, ok := actor.Whereis("main")
	if ok {
		actor.Send(p, "top-level")
	}
}

func topLevel() func() { return second }

func pick(a Job, b Job, first bool) Job {
	if first {
		return a
	}
	return b
}

func start(j Job) actor.PID {
	println("starting", j.Name)
	return actor.Spawn(j.Run)
}

func main() {
	actor.Register("main", actor.Self())
	me := actor.Self()
	greeting := "literal"
	run := func() { actor.Send(me, greeting) }
	a := Job{Name: "a", Run: run}
	b := Job{"b", topLevel()}

	// Spawn the second job only after the first one reported back.
	start(pick(a, b, true))
	println(actor.Receive())
	start(pick(a, b, false))
	println(actor.Receive())

	// A job travels inside a message; the closure is copied with it.
	launcher := actor.Spawn(func() {
		switch m := actor.Receive().(type) {
		case Job:
			start(m)
		}
	})
	actor.Send(launcher, Job{"c", func() { actor.Send(me, "via message") }})
	println(actor.Receive())
}
