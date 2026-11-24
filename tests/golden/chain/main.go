package main

import "gogo/actor"

type Result struct{ N int }

func fib(n int) int {
	if n < 2 {
		return n
	}
	return fib(n-1) + fib(n-2)
}

// node spawns the next link and waits for its result: all 10001 actors are
// alive at once, each blocked in Receive with at most one message queued.
func node(parent actor.PID, n int) {
	if n == 0 {
		actor.Send(parent, Result{0})
		return
	}
	me := actor.Self()
	actor.Spawn(func() {
		node(me, n-1)
	})
	switch m := actor.Receive().(type) {
	case Result:
		actor.Send(parent, Result{m.N + fib(10)})
	}
}

func main() {
	me := actor.Self()
	actor.Spawn(func() {
		node(me, 10000)
	})
	switch m := actor.Receive().(type) {
	case Result:
		println(m.N)
	}
}
