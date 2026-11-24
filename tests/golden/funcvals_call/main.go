package main

import "gogo/actor"

type Request struct {
	Path string
	N    int
}

type Response struct {
	Status int
	Body   string
}

// Server holds a handler of the http.Serve shape.
type Server struct {
	Name    string
	Handler func(Request) Response
}

// Rec lets a func value reach itself: recursion through a func value.
type Rec struct {
	F func(Rec, int) int
}

func hello(r Request) Response { return Response{200, "hello " + r.Path} }

func divmod(a, b int) (int, int) { return a / b, a % b }

// fact recurses through a func value of itself.
func fact(n int) int {
	if n <= 1 {
		return 1
	}
	f := fact
	return n * f(n-1)
}

func apply(f func(int, int) (int, int), a, b int) (int, int) { return f(a, b) }

func adder(k int) func(int) int { return func(x int) int { return x + k } }

func compose(f, g func(int) int) func(int) int {
	return func(x int) int { return g(f(x)) }
}

func describe(v any) string {
	switch x := v.(type) {
	case int:
		return "int"
	case string:
		return "string " + x
	}
	return "other"
}

func main() {
	me := actor.Self()
	prefix := "v1"
	srv := Server{"api", func(r Request) Response {
		if r.N < 0 {
			return Response{400, "bad " + r.Path}
		}
		return Response{200, prefix + r.Path}
	}}

	// The handler is called inside a spawned actor (the closure, with its
	// captured prefix, was copied into the child's heap).
	actor.Spawn(func() {
		actor.Send(me, srv.Handler(Request{"/x", 1}))
	})
	println(actor.Receive())

	// The server travels inside a message and its handler is called there.
	worker := actor.Spawn(func() {
		switch m := actor.Receive().(type) {
		case Server:
			actor.Send(me, m.Handler(Request{"/msg", 2}))
		}
	})
	actor.Send(worker, srv)
	println(actor.Receive())

	// Handlers in a slice: a top-level function and a closure.
	hs := []func(Request) Response{hello, srv.Handler}
	for i := 0; i < len(hs); i++ {
		r := hs[i](Request{"/y", -1})
		println(r.Status, r.Body)
	}

	// Multiple results through a func value.
	q, m := apply(divmod, 17, 5)
	println(q, m)
	d := divmod
	a, b := d(9, 4)
	println(a, b)
	d(1, 1) // results discarded

	// Closures with parameters, returned and composed.
	inc := adder(1)
	dbl := func(x int) int { return x * 2 }
	println(compose(inc, dbl)(5), adder(10)(5))
	println(func(a, b string) string { return a + b }("x", "y"))

	// Recursion through func values.
	println(fact(10))
	r := Rec{func(r Rec, n int) int {
		if n <= 1 {
			return 1
		}
		return n * r.F(r, n-1)
	}}
	println(r.F(r, 6))

	// int64, any and multi-result parameters of func values.
	sq := func(x int64) int64 { return x * x }
	println(sq(3000000000))
	desc := describe
	println(desc(1), desc("s"))
	split := func(s string) (string, string, int) { return s[:1], s[1:], len(s) }
	h, t, n := split("gogo")
	println(h, t, n)

	// f(g()) into a func value; named results in a literal.
	sum := func(a, b int) int { return a + b }
	println(sum(divmod(17, 5)))
	minmax := func(a, b int) (lo, hi int) {
		lo, hi = a, b
		if a > b {
			lo, hi = b, a
		}
		return
	}
	lo, hi := minmax(9, 2)
	println(lo, hi)
}
