package main

import "gogo/actor"

// Main greets its argument.
func Main(arg any) {
	switch s := arg.(type) {
	case string:
		println("hello from module", s)
	}
}

// Wait prints after its first message.
func Wait(arg any) {
	actor.Receive()
	println("wait done")
}

// helper is not exported: no entry.
func helper(arg any) { println("never") }

// Two takes two parameters: no entry.
func Two(a, b any) { println("never") }

func main() {
	println("main of module")
}
