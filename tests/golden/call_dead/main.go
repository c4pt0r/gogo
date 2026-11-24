package main

import "gogo/actor"

func main() {
	dead := actor.Spawn(func() {})
	actor.Sleep(10)
	t0 := actor.Now()
	reply, ok := actor.Call(dead, 1, 5000)
	println(ok)
	println(actor.Now()-t0 < 1000)
	_ = reply
}
