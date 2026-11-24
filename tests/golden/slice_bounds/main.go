package main

import "gogo/actor"

func get(s []int, i int) int { return s[i] }

func put(s []int, i, v int) { s[i] = v }

func sub(s []int, lo, hi int) []int { return s[lo:hi] }

func mk(n, c int) []int { return make([]int, n, c) }

func main() {
	s := make([]int, 2, 4)
	println(get(s, 1), len(sub(s, 1, 4)), len(sub(s, 4, 4)), len(mk(0, 0)))
	actor.Spawn(func() {
		println(get(s, 2)) // index == len: badcode in get
	})
	actor.Spawn(func() {
		put(s, -1, 0) // badcode in put
	})
	actor.Spawn(func() {
		println(len(sub(s, 0, 5))) // hi > cap: badcode in sub
	})
	actor.Spawn(func() {
		println(len(mk(3, 2))) // len > cap: badcode in mk
	})
	actor.Spawn(func() {
		var n []int
		println(get(n, 0)) // nil: badcode in get
	})
	actor.Spawn(func() {
		n := 0 - 1
		println(len(s[n:])) // negative lo: badcode in the closure
	})
	actor.Sleep(50)
	println("ok")
}
