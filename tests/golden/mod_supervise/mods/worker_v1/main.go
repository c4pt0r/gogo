package main

import "gogo/actor"

// Work announces itself to arg (a PID), then echoes its messages; the 2nd
// one crashes it.
func Work(arg any) {
	switch host := arg.(type) {
	case actor.PID:
		actor.Send(host, actor.Self())
	}
	n := 0
	for {
		actor.Receive()
		n++
		if n == 2 {
			actor.Exit(42)
		}
		println("v1 msg", n)
	}
}

func main() {}
