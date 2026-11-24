package main

import "gogo/actor"

type P struct{ X, Y int }

type Box struct {
	V any
}

// show reports which kind of value v holds.
func show(v any) {
	switch x := v.(type) {
	case P:
		println("struct", x.X, x.Y)
	case int:
		println("int", x)
	case string:
		println("string", x)
	case bool:
		println("bool", x)
	case actor.PID:
		println("pid", x == actor.Self())
	default:
		println("other", x)
	}
}

func wrap(v any) any { return v }

// noisy proves that a type switch evaluates its operand once.
func noisy() any {
	println("eval")
	return 5
}

func main() {
	var a any = P{1, 2}
	var b any = 42
	c := Box{"hi"}
	var d any
	d = true
	e := Box{V: actor.Self()}
	show(a)
	show(b)
	show(c.V)
	show(d)
	show(e.V)

	show(wrap(1 << 30))
	var empty string
	show(empty)
	var z any
	show(z)
	switch noisy().(type) {
	case bool:
		println("bool?")
	case int, string:
		println("int or string")
	}
	println(a, b, c, d)

	actor.Send(actor.Self(), Box{a})
	switch m := actor.Receive().(type) {
	case Box:
		show(m.V)
	}
}
