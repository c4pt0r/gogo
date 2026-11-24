package main

type Pair struct {
	A, B int
}

func sum(n int) int {
	s := 0
	for i := 0; i < n; i++ {
		s += i
	}
	return s
}

func depth(n int) int {
	if n == 0 {
		return 0
	}
	return depth(n-1) + 1
}

func main() {
	// 200000 iterations: many time slices, results beyond 31 bits are boxed.
	println(sum(200000)) // 19999900000 wraps to -1474936480
	// Boxed values flow through registers, comparisons and structs, and
	// survive GCs triggered by later allocations.
	big := 1 << 30
	p := Pair{big + big, -big - big - 1}
	n := 0
	for i := 0; i < 5000; i++ {
		q := Pair{i * big, i}
		if q.B == i {
			n++
		}
	}
	println(p, p.A == -2147483648, p.B, n) // {-2147483648 2147483647} true 2147483647 5000
	println(depth(3000))                   // 3000
}
