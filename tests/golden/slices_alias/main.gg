package main

import "gogo/actor"

type Holder struct {
	A, B []int
}

// churn allocates garbage until the heap has been collected many times.
func churn(n int) int {
	t := 0
	for i := 0; i < n; i++ {
		s := "garbage" + "-" + "garbage"
		g := []int{i, i, i, i}
		t += len(s) + len(g)
	}
	return t
}

func main() {
	// Two headers on one array: a write through one is seen by the other.
	base := make([]int, 4, 8)
	x := base[1:3]
	y := base[2:4]
	x[1] = 42
	println(base[2], y[0])

	// append within cap writes into the shared array; beyond cap detaches.
	a := make([]int, 2, 3)
	b := append(a, 10)
	c := append(a, 20) // overwrites b[2]
	println(b[2], c[2], len(a), cap(b))
	d := append(b, 30) // cap 3 -> new array
	d[0] = 99
	println(a[0], b[0], d[0], cap(d), len(d))

	// Several values: growth is decided once. If they do not all fit,
	// none is written into the old array.
	ma := []int{1, 2, 3, 4}
	mb := ma[:2]
	mb = append(mb, 99)   // within cap: ma[2] = 99
	mb = append(mb, 5, 6) // len 3, cap 4, needs 5: a new array
	println(ma[2], ma[3], len(mb), mb[3], mb[4])
	mc := append(mb[:1], 7, 8) // fits: written in place, seen through mb
	println(mb[1], mb[2], len(mc), cap(mc) == cap(mb))
	ba := []byte("abcd")
	bb := ba[:2]
	bb = append(bb, 'X')
	bb = append(bb, 'Y', 'Z')
	println(string(ba), string(bb))
	bc := append(ba[:1], 'Q', 'R')
	println(string(ba), string(bc))

	// []byte too, and a struct holding slices sees writes made elsewhere.
	buf := []byte("hello")
	h := buf[1:3]
	h[0] = 'E'
	println(string(buf), string(h))
	hold := Holder{A: base, B: x}
	base[1] = 7
	println(hold.A[1], hold.B[0])

	// The same in an actor with a small heap that collects many times.
	me := actor.Self()
	actor.SpawnOpt(func() {
		s := make([]int, 3, 6)
		t := s[1:5]
		u := append(s, 5) // within cap: shares with s and t
		for i := 0; i < 50; i++ {
			churn(20)
			t[0] = t[0] + 1
			u = append(u, i) // grows and detaches u
		}
		println(s[1], t[0], len(u), u[1], u[3], u[53])
		bs := []byte("abc")
		bt := bs[:2]
		churn(200)
		bt[1] = 'B'
		println(string(bs))
		actor.Send(me, 1)
	}, actor.Options{MaxMemory: 2048})
	actor.Receive()
	println("done")
}
