// Counts the posts to "count" (Kind = producer * 1000000 + sequence from 1)
// and checks each producer's posts arrive in order; Kind -1 reports.
package main

import (
	"gogo/actor"
	"gogo/port"
)

func main() {
	actor.Register("count", actor.Self())
	println("ready")
	last := make([]int, 8)
	n := 0
	bad := 0
	for {
		switch p := actor.Receive().(type) {
		case port.Post:
			if p.Kind < 0 {
				println("count", n, "bad", bad)
				return
			}
			t := p.Kind / 1000000
			s := p.Kind % 1000000
			if t >= 8 || s <= last[t] {
				bad++
			} else {
				last[t] = s
			}
			n++
		}
	}
}
