package main

import "gogo/actor"

type Result struct{ N int }

func fib(n int) int {
	if n < 2 {
		return n
	}
	return fib(n-1) + fib(n-2)
}

// 10000 actors in 100 batches of 100: a mailbox holds at most 65535 words
// of queued messages (about 9000 Result messages on a 64-bit host).
func main() {
	me := actor.Self()
	sum := 0
	for batch := 0; batch < 100; batch++ {
		for i := 0; i < 100; i++ {
			actor.Spawn(func() {
				actor.Send(me, Result{fib(10)})
			})
		}
		for i := 0; i < 100; i++ {
			switch m := actor.Receive().(type) {
			case Result:
				sum += m.N
			}
		}
	}
	println(sum)
}
