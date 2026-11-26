package main

import "gogo/actor"

func build(n int) string {
	s := ""
	for i := 0; i < 10; i++ {
		s = s + "0123456789" // 10 bytes each
	}
	return s[:n]
}

func main() {
	me := actor.Self()
	actor.SpawnOpt(func() {
		last := ""
		for i := 0; i < 500; i++ {
			last = ""
			for j := 0; j < 5; j++ {
				last = last + "0123456789" // a 50-byte string, built piece by piece
			}
		}
		println(len(last), last[49])
		actor.Send(me, len(last))
	}, actor.Options{MaxMemory: 4096})
	switch m := actor.Receive().(type) {
	case int:
		println(m)
	}
	println(len(build(50)))
}
