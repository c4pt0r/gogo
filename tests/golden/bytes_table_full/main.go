// The handle table (64 entries) filled by idle actors: four sinks each
// receive and discard 16 off-heap Bytes and block in Receive again, never
// collecting, so their stale handles stay on their holder lists. A fresh
// worker's Freeze finds the table full: it collects those holders and
// succeeds instead of dying OutOfMemory (M4 final review, finding 1).
package main

import (
	"gogo/actor"
	"gogo/bytes"
)

func big(c byte, n int) bytes.Bytes {
	b := bytes.NewBuffer(n)
	for i := 0; i < n; i++ {
		b.WriteByte(c)
	}
	return b.Freeze()
}

func sink() {
	for {
		actor.Receive()
	}
}

func main() {
	var sinks []actor.PID
	for i := 0; i < 4; i++ {
		sinks = append(sinks, actor.Spawn(sink))
	}
	for k := 0; k < 16; k++ {
		for _, s := range sinks {
			actor.Send(s, big('a', 65))
		}
		actor.Sleep(1)
	}
	println("sent")
	worker := actor.Spawn(func() {
		x := big('w', 100)
		println("worker ok", x.Len())
	})
	r := actor.Monitor(worker)
	actor.Receive()
	println("done", r == r)
}
