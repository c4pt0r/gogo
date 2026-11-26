package main

func tail(s string) string { return s[1:] }

func firstTwo(s string) (byte, byte) { return s[0], s[1] }

func main() {
	a := "foo"
	b := ""
	for i := 0; i < 3; i++ {
		b = b + "ab" // heap + const
	}
	c := a + b // heap + heap
	println(c, len(c))
	println(len(""), len(a), len("你好"), len("héllo"))

	// index
	println(c[0], c[3], c[len(c)-1])
	x, y := firstTwo(c)
	println(x, y, x+y)
	println(int(c[1]) + 1000)

	// slices of every form
	println(c[2:5], c[:3], c[4:], c[:], c[3:3], len(c[9:]))
	println(tail("hello"), tail(tail(c)))
	s := c[1:8]
	println(s, s[1:3], len(s))

	// empty string edges
	e := ""
	println(len(e), len(e+e), e+"x" == "x", "x"+e == "x", e == "", e[0:0] == "")
	println(len(e[:]), e < "a", e < e, e <= e)
	var z string
	println(z == "", len(z), z+"q")

	// equality across const / heap
	h := "ab" + "c"
	k := a[:0] + "abc"
	println(h == "abc", k == "abc", k == h, k != "abd", "abc" != k)
	println(a == b, a != b)

	// ordering, including prefixes
	println("a" < "b", "b" < "a", "a" <= "a", "a" < "a")
	println("ab" < "abc", "abc" < "ab", "abc" > "ab", "ab" >= "ab", "ab" >= "abc")
	println("" < "a", "a" > "", "Z" < "a", "你" > "z")
	p := a[:2]
	q := a[:2] + "o"
	println(p < q, q > p, q <= a, q >= a, a < p)

	// range over bytes
	sum := 0
	for i, ch := range "abc" {
		sum += i*1000 + int(ch)
	}
	println(sum)
	n := 0
	for range c {
		n++
	}
	println(n)
	for i := range "xyz" {
		print2(i)
	}
	cnt := 0
	for _, ch := range "你" { // three bytes
		cnt += int(ch)
	}
	println(cnt)

	// concatenation in a loop, += and a long result
	acc := ""
	for i := 0; i < 5; i++ {
		acc += "xyz"
	}
	println(acc, len(acc))
	var big string
	for i := 0; i < 40; i++ {
		big += "0123456789"
	}
	println(len(big), big[399], big[100:103])
}

func print2(i int) { println("i", i) }
