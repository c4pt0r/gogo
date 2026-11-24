package main

import (
	"gogo/actor"
	"gogo/bytes"
)

type Pair struct{ A, B int }

func divmod(a, b int) (int, int) { return a / b, a % b }

func three(n int) (int, string, bool) { return n * 2, "three", n > 0 }

// named results, set then returned by a bare return.
func digits(n int) (tens, ones int) {
	tens = n / 10
	ones = n % 10
	return
}

// named results: an explicit return, then the zero values, then a bare one.
func classify(n int) (s string, ok bool) {
	if n < 0 {
		return "neg", false
	}
	if n == 0 {
		return
	}
	s = "pos"
	ok = true
	return
}

func swap(a, b int) (int, int) { return b, a }

// return g() where g has several results.
func forward(a, b int) (int, int) { return swap(a, b) }

func sum(a, b int) int { return a + b }

func raw(x int64) (int64, int64) { return x, x * 2 }

// return g() with an int64 result going into any: boxed.
func boxed(x int64) (any, int64) { return raw(x) }

// heap values through the multi-result return.
func pairs(i int) (Pair, Pair) { return Pair{i, i + 1}, Pair{i * 2, i * 3} }

// named results of every kind left at their zero values.
func zeros() (i int64, p actor.PID, st Pair, a any, rf actor.Ref, bs []byte, x bytes.Bytes) {
	return
}

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

func main() {
	q, r := divmod(17, 5)
	println(q, r)
	a, b, c := three(4)
	println(a, b, c)
	t, o := digits(42)
	println(t, o)
	s, ok := classify(-3)
	println(s, ok)
	s, ok = classify(0)
	println(s, ok)
	s, ok = classify(7)
	println(s, ok)

	q, r = swap(q, r)
	println(q, r)
	x, y := forward(1, 2)
	println(x, y)
	println(sum(divmod(17, 5)))
	println(sum(forward(10, 20)))

	_, r2 := divmod(9, 4)
	q3, _ := divmod(9, 4)
	println(r2, q3)
	divmod(1, 1) // results discarded

	var v any
	var n int64
	v, n = boxed(5)
	show(v)
	println(n)

	// Enough struct results to collect garbage between calls.
	total := 0
	for i := 0; i < 3000; i++ {
		p1, p2 := pairs(i)
		total = total + p1.A + p1.B + p2.A + p2.B - 7*i
	}
	println(total)

	// Zero named results.
	var zp actor.PID
	var zr actor.Ref
	zi, zpid, zst, za, zref, zbs, zx := zeros()
	show(za)
	println(zi, zpid == zp, zst.A, zst.B, zref == zr, zbs == nil, len(zbs), zx.Len(), zx.Equal(bytes.FromString("")))

	// Results inside actors spawned from closures.
	done := actor.Self()
	actor.Spawn(func() {
		d, m := divmod(100, 7)
		tt, oo := digits(d)
		actor.Send(done, Pair{tt*10 + oo, m})
	})
	msg := actor.Receive()
	switch p := msg.(type) {
	case Pair:
		println("actor", p.A, p.B)
	}
}
