// A first actor: main spawns a greeter, sends it a name, and waits for the
// greeting to come back.
package main

import "gogo/actor"

// Greet asks the greeter to greet Name and answer to From.
type Greet struct {
	From actor.PID
	Name string
}

func greeter() {
	for {
		switch m := actor.Receive().(type) {
		case Greet:
			actor.Send(m.From, "hello, "+m.Name)
		}
	}
}

func main() {
	g := actor.Spawn(greeter)
	actor.Send(g, Greet{From: actor.Self(), Name: "world"})
	println(actor.Receive())
}
