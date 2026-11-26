// Bit operations on int, int64 and byte.
package main

const (
	FlagRead  = 1 << iota // 1
	FlagWrite             // 2
	FlagExec              // 4
)

func main() {
	perm := FlagRead | FlagWrite
	println(perm&FlagWrite != 0, perm&FlagExec != 0)
	perm |= FlagExec
	perm &^= FlagWrite // clear a bit
	println(perm)

	// >> is an arithmetic shift: the sign is kept. A shift by the width
	// or more gives 0 (or -1 for a negative value shifted right).
	x := -16
	one := 1
	println(x>>2, one<<30, x>>40, one<<40)
	println(^0, 0xF0^0xFF)

	// byte results are cut to 8 bits after every operation.
	var b byte = 0x81
	println(b<<1, b>>1, ^b)

	// int64 has the full 64 bits.
	var w int64 = 1
	w = w<<62 | 5
	println(w, w>>60, w&0xFF)

	// Pack and unpack an IPv4 address held in an int, as gogo/net does.
	// Addresses from 128.0.0.0 up are negative ints.
	ip := ipv4(192, 168, 1, 42)
	println(ip, ip>>24&0xFF, ip>>16&0xFF, ip>>8&0xFF, ip&0xFF)
}

func ipv4(a, b, c, d int) int { return a<<24 | b<<16 | c<<8 | d }
