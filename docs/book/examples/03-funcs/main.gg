// Functions: several results, named results, function values, closures,
// and the rule that a captured variable must not change.
package main

func divmod(a, b int) (int, int) {
	return a / b, a % b
}

// minmax has named results and a bare return.
func minmax(xs []int) (lo, hi int) {
	lo, hi = xs[0], xs[0]
	for _, x := range xs {
		if x < lo {
			lo = x
		}
		if x > hi {
			hi = x
		}
	}
	return
}

// apply takes a function value.
func apply(f func(int) int, xs []int) []int {
	out := make([]int, 0, len(xs))
	for _, x := range xs {
		out = append(out, f(x))
	}
	return out
}

// adder returns a closure. step is captured by value when the closure is
// made, and is never assigned again.
func adder(step int) func(int) int {
	return func(x int) int { return x + step }
}

func double(x int) int { return x * 2 }

func show(xs []int) {
	s := ""
	for i, x := range xs {
		if i > 0 {
			s = s + " "
		}
		s = s + itoa(x)
	}
	println(s)
}

// itoa is a small decimal formatter (gogo/strconv.Itoa does the same).
func itoa(n int) string {
	if n == 0 {
		return "0"
	}
	neg := n < 0
	if neg {
		n = -n
	}
	b := []byte{}
	for n > 0 {
		b = append([]byte{byte('0' + n%10)}, b...)
		n /= 10
	}
	if neg {
		return "-" + string(b)
	}
	return string(b)
}

func main() {
	q, r := divmod(17, 5)
	println("divmod", q, r)
	lo, hi := minmax([]int{4, -2, 9, 3})
	println("minmax", lo, hi)

	xs := []int{1, 2, 3}
	show(apply(double, xs))
	show(apply(adder(10), xs))
	show(apply(func(x int) int { return x * x }, xs))

	// A closure that needs changing state gets it as an argument and
	// returns the new value instead of assigning a captured variable.
	count := func(n int, s string) int { return n + len(s) }
	total := 0
	for _, w := range []string{"go", "gogo", "actor"} {
		total = count(total, w)
	}
	println("letters", total)
}
