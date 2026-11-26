package main

import "gogo/actor"

// Work (version 1) announces itself to arg (a PID) and counts its messages;
// it crashes on the third.
func Work(arg any) {
	switch host := arg.(type) {
	case actor.PID:
		actor.Send(host, actor.Self())
	}
	n := 0
	for {
		actor.Receive()
		n++
		if n == 3 {
			println("worker v1: crashing")
			actor.Exit(42)
		}
		println("worker v1: message", n)
	}
}

func main() {}
