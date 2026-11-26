package main

type D int64

func shl(x, n int) int           { return x << n }
func shr(x, n int) int           { return x >> n }
func shl64(x int64, n int) int64 { return x << n }

func main() {
	// int, basic operators
	a, b := 0x0F0F, 0x00FF
	println(a&b, a|b, a^b, a&^b)
	println(1<<8|3, 6&3, 6^3)

	// int, shifts: boxing at 31 bits, clamping, arithmetic right shift
	println(shl(1, 30), shl(1, 31), shl(1, 32), shl(-1, 31), shl(7, 0))
	println(shr(-8, 1), shr(-1, 40), shr(5, 32), shr(-5, 32), shr(1<<30, 29))
	big := 0x7fffffff
	m1 := -1
	println(big&m1, big|m1, big^m1, big&^m1, big>>16)
	x := 1 << 30
	println(x|x>>1, x^x, x&^(x>>1))

	// mixed count types: int << int64 (incl. a boxed count), int64 << byte
	n64 := int64(3)
	huge := int64(1) << 40
	println(5<<n64, 5>>n64, 5<<huge, -5>>huge)
	var bc byte = 33
	var w int64 = 1
	println(w<<bc, w<<n64, w>>bc, -w>>bc)
	var u byte = 4
	println(shl64(3, 62), shl64(3, 63), shl64(3, 64), shl64(-1, 63)>>63, w<<u)

	// untyped constant left operand takes the context type
	n := 20
	var z int64 = 1 << n
	println(1<<n, z, 1<<(n+11), z<<20, int64(1)<<(n+40))
	var z62 int64 = 1 << (n + 42)
	println(z62, z62>>62, z62<<1, (z62<<1)>>63)

	// int64
	p, q := int64(0x0F0F0F0F0F), int64(0xFF00FF00FF)
	println(p&q, p|q, p^q, p&^q)
	println(p<<20, p>>8, ^p, -p>>4)
	var d D = 6
	d <<= 62
	println(d, d>>62, ^d, d&^d, d|1)

	// ^x on each type
	i5, l5, b5 := 5, int64(5), byte(5)
	println(^i5, ^(i5 - 5), ^l5, ^(l5 << 40), ^b5, ^(b5 << 5))

	// compound assignment
	c := 0xF0
	c &= 0x3C
	println(c)
	c |= 0x101
	println(c)
	c ^= 0xFF
	println(c)
	c &^= 0x0F
	println(c)
	c <<= 4
	println(c)
	c >>= 5
	println(c)
	e := int64(1)
	e <<= 40
	e |= 5
	e &^= 4
	e ^= 1 << 41
	e >>= 3
	e &= 0xFFFFFFFFFF
	println(e)

	// byte stays in 0..255
	var by byte = 200
	var mk byte = 0x0F
	println(by<<1, by<<8, by>>1, ^by, by&^mk, by&mk, by|mk, by^mk)
	var cnt byte = 3
	println(by<<cnt, by>>cnt, ^by>>cnt)
	by <<= 1
	println(by)
	by ^= 0xFF
	println(by)
	by |= 0x80
	by &^= 0x01
	println(by)
	by >>= 9
	println(by)
	by = 0xAB
	by <<= 4
	println(by)
	by--
	by = ^by
	println(by)

	// []byte elements
	s := []byte{0xF1, 0x0F, 0xFF, 0x80, 0x55}
	s[0] <<= 4
	s[1] |= 0xF0
	s[2] ^= 0x0F
	s[3] >>= 7
	s[4] &^= 0x44
	println(s[0], s[1], s[2], s[3], s[4])
	s[0] <<= 1
	s[1] &= 0x3C
	println(s[0], s[1])
	i := 2
	s[i] <<= 9
	println(s[i])

	// []int and []int64 elements
	t := []int{1, 1 << 30, -1}
	t[0] <<= 31
	t[1] |= 1
	t[2] >>= 3
	println(t[0], t[1], t[2])
	tl := []int64{1, -1}
	tl[0] <<= 62
	tl[1] ^= 1 << 40
	println(tl[0], tl[1])

	// precedence and bit tests
	flags := 0
	flags |= 1 << 3
	flags |= 1 << 5
	println(flags, flags&(1<<3) != 0, flags&(1<<4) != 0, flags>>3&1)
}
