// Sleeps between two lines.
package main

import "gogo/actor"

func main() {
	println("a")
	actor.Sleep(50)
	println("b")
}
