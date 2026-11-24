package main

import (
	"gogo/actor"
	"gogo/bytes"
)

type Point struct{ X, Y int }

type Job struct {
	Name string
	Run  func(int) int
}

// counter returns a closure over its parameter and a local: captures are
// the hidden function's leading parameters, the call's arguments follow.
func counter(start int, step int) func(int) (int, int) {
	base := start * 10
	return func(n int) (int, int) { return base + n*step, n }
}

// curry nests closures: the inner one captures the outer one's parameter.
func curry(a int) func(int) func(int) int {
	return func(b int) func(int) int {
		return func(c int) int { return a*100 + b*10 + c }
	}
}

func run(j Job, x int) int { return j.Run(x) }

func main() {
	me := actor.Self()

	// Captures of every kind, used together with the arguments.
	s := "str"
	big := int64(1) << 40
	p := Point{3, 4}
	xs := []int{7, 8, 9}
	bs := bytes.FromString("bytes")
	f := func(i int, tag string) string {
		return tag + s + bs.String()
	}
	g := func(i int) int64 { return big + int64(xs[i]+p.X*p.Y) }
	println(f(0, "<"), g(1))

	c := counter(4, 3)
	v, n := c(5)
	println(v, n)
	println(curry(1)(2)(3))
	add := curry(9)
	println(add(8)(7), add(0)(0))

	// Closures carried in a Job run in spawned actors, which call them
	// with their own arguments; the parent keeps its copies.
	k := 1000
	jobs := []Job{{"plus", func(x int) int { return x + k }}, {"sq", func(x int) int { return x * x }}}
	for i := 0; i < len(jobs); i++ {
		j := jobs[i]
		actor.Spawn(func() {
			actor.Send(me, j.Name)
			actor.Send(me, run(j, 12))
		})
		println(actor.Receive(), actor.Receive())
	}
	println(run(jobs[0], 1))

	// A nil func value: calling it crashes the caller with badcode.
	actor.Spawn(func() {
		var h func(int) int
		println(h(1))
	})
	actor.Spawn(func() {
		var j Job
		println(run(j, 2))
	})
	actor.Spawn(func() {
		hs := make([]func(Point) (int, int), 1)
		a, b := hs[0](p)
		println(a, b)
	})
	actor.Sleep(50)
	println("ok")
}
