// Control flow: if/else chains, the three for forms, range over slices and
// strings, break and continue. There is no expression switch: use an
// if/else chain (a type switch exists, see the actor chapter).
package main

func classify(n int) string {
	if n < 0 {
		return "negative"
	} else if n == 0 {
		return "zero"
	} else if n < 10 {
		return "small"
	}
	return "large"
}

func main() {
	for _, n := range []int{-5, 0, 7, 42} {
		println(n, classify(n))
	}

	// The C-style loop, the while-style loop and the endless loop.
	sum := 0
	for i := 1; i <= 10; i++ {
		if i%2 == 0 {
			continue
		}
		sum += i
	}
	println("odd sum", sum)

	n := 27
	steps := 0
	for n != 1 {
		if n%2 == 0 {
			n = n / 2
		} else {
			n = 3*n + 1
		}
		steps++
	}
	println("collatz steps", steps)

	i := 0
	for {
		i++
		if i == 5 {
			break
		}
	}
	println("broke at", i)

	// range over a string gives byte indexes and byte values.
	for i, c := range "gogo" {
		if c == 'o' {
			println("o at", i)
		}
	}

	// An if statement may start with a short declaration.
	if half := sum / 2; half > 10 {
		println("half", half)
	}
}
