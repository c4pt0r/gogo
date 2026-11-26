package main

import "gogo/actor"

type Stamp struct {
	Name string
	At   int64
	N    int
}

// show reports which kind of value v holds.
func show(v any) {
	switch x := v.(type) {
	case int64:
		println("int64", x)
	case int:
		println("int", x)
	default:
		println("other")
	}
}

func add(a, b int64) int64 { return a + b }

func main() {
	var a int64 = 1 << 40
	println(a, a*3, -a)

	m := int64(-9223372036854775807) - 1
	println(m / -1, int64(7) % -1)
	one := int64(-1)
	println(m/one, m%one, -m, m-1)

	x := int64(9223372036854775807)
	x++
	println(x)

	big := int64(1<<32 + 5)
	n := -3
	println(int(big), int64(int(-3)), int64(n), int(int64(n)))
	neg := int64(-1) << 40
	println(int(neg - 7))

	var v any = int64(5)
	var w any = 5
	show(v)
	show(w)

	// Arithmetic, compound assignment, comparison.
	var z int64
	z += 100
	z -= 1
	z *= -3
	z /= 2
	z %= 7
	println(z, int64(-7)/2, int64(-7)%2)
	p, q := int64(1)<<35, int64(1)<<35+1
	println(p < q, p <= q, p > q, p >= q, p == q, p != q, p == q-1)
	println(add(p, q))

	// Many boxed results: the GC must keep the live ones intact.
	var sum int64
	for i := 0; i < 20000; i++ {
		sum += int64(i) * 1000000
	}
	println(sum)

	// int64 inside a struct, sent to self.
	actor.Send(actor.Self(), Stamp{"t", 1<<50 + 3, 7})
	switch s := actor.Receive().(type) {
	case Stamp:
		println(s.Name, s.At, s.N)
	}
	// A bare int64 as a message.
	actor.Send(actor.Self(), a)
	show(actor.Receive())

	// Captured by a spawned actor.
	me := actor.Self()
	actor.Spawn(func() {
		actor.Send(me, a+1)
	})
	show(actor.Receive())
}
