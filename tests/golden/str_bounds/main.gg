package main

import "gogo/actor"

func idx(s string, i int) byte { return s[i] }

func cut(s string, a, b int) string { return s[a:b] }

func main() {
	me := actor.Self()
	s := "abc"
	println(idx(s, 2), cut(s, 1, 3), cut(s, 3, 3))
	actor.Spawn(func() {
		println(idx(s, 5)) // crashes: badcode in main.idx
	})
	actor.Spawn(func() {
		println(cut(s, 2, 1)) // crashes: badcode in main.cut
	})
	actor.Spawn(func() {
		println(cut(s, 0, 4))
	})
	actor.Spawn(func() {
		println(idx(s, -1))
	})
	actor.Sleep(50)
	actor.Send(me, 1)
	switch m := actor.Receive().(type) {
	case int:
		println(m)
	}
}
