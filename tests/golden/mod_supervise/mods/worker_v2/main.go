package main

import "gogo/actor"

// Work is version 2: it never crashes.
func Work(arg any) {
	switch host := arg.(type) {
	case actor.PID:
		actor.Send(host, actor.Self())
	}
	for {
		actor.Receive()
		println("v2")
	}
}

func main() {}
