// A module: an ordinary gogo program built on its own. Its exported
// functions with the signature func() or func(arg any) are its entries.
package main

import "gogo/actor"

// Greet is an entry: it tells the PID it was started with who it is.
func Greet(arg any) {
	switch host := arg.(type) {
	case actor.PID:
		actor.Send(host, "hello from a module")
	}
}

func main() {}
