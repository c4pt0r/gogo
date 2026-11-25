package main

import "gogo/actor"

// Work (version 2) is the fixed worker: it counts messages and never crashes.
func Work(arg any) {
	switch host := arg.(type) {
	case actor.PID:
		actor.Send(host, actor.Self())
	}
	n := 0
	for {
		actor.Receive()
		n++
		println("worker v2: message", n)
	}
}

func main() {}
