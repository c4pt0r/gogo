// Sleeps 2 s five times in simulated time, printing the clock each time.
package main

import "gogo/actor"

func main() {
	println("start", actor.Now())
	for i := 0; i < 5; i++ {
		actor.Sleep(2000)
		println("tick", actor.Now())
	}
}
