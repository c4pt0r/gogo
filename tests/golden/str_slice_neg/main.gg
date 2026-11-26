package main

import "gogo/actor"

func tail(s string, n int) string { return s[2:n] }

func main() {
	s := "hello"
	println(s[2:], s[:], s[:0], s[5:], len(s[0:]))
	println(tail(s, 4))
	actor.Spawn(func() {
		n := 0 - 1
		println(s[2:n]) // crashes: badcode (not the suffix)
	})
	actor.Spawn(func() {
		println(tail(s, -1)) // crashes: badcode in tail
	})
	actor.Sleep(50)
	println("ok")
}
