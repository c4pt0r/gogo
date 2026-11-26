// Prints every port.Post sent to the name "echo"; Kind -1 ends it.
package main

import (
	"gogo/actor"
	"gogo/port"
)

func main() {
	actor.Register("echo", actor.Self())
	println("ready")
	for {
		switch p := actor.Receive().(type) {
		case port.Post:
			if p.Kind == -1 {
				println("bye")
				return
			}
			if p.Data.Len() <= 32 {
				println("post", p.Kind, p.Data.Len(), p.Data.String())
			} else {
				println("post", p.Kind, p.Data.Len(), p.Data.At(0), p.Data.At(p.Data.Len()-1))
			}
		}
	}
}
