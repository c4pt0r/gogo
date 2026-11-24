// Package strconv is the gogo number conversion library: a small subset of
// Go's strconv written in the gogo subset and compiled into every image that
// imports it. The subset has no error values, so the parse functions report
// failure with a bool.
package strconv

const digits = "0123456789abcdefghijklmnopqrstuvwxyz"

// Itoa is FormatInt64(int64(i), 10).
func Itoa(i int) string { return FormatInt64(int64(i), 10) }

// Atoi parses s as a decimal int, accepting an optional sign. ok is false
// if s is not a valid number or does not fit in 32 bits.
func Atoi(s string) (n int, ok bool) {
	v, ok := ParseInt64(s, 10)
	if !ok || v < -2147483648 || v > 2147483647 {
		return 0, false
	}
	return int(v), true
}

// FormatInt64 returns the string form of i in the given base, 2 <= base <=
// 36, using lower case letters for digits above 9. For any other base it
// returns "" (Go panics; the subset has no panic).
func FormatInt64(i int64, base int) string {
	if base < 2 || base > 36 {
		return ""
	}
	buf := make([]byte, 65) // 64 binary digits and a sign
	p := len(buf)
	b := int64(base)
	// Work with a non-positive value so that the minimum int64 needs no
	// special case.
	n := i
	if n > 0 {
		n = -n
	}
	for {
		p--
		buf[p] = digits[int(-(n % b))]
		n /= b
		if n == 0 {
			break
		}
	}
	if i < 0 {
		p--
		buf[p] = '-'
	}
	return string(buf[p:])
}

// ParseInt64 parses s as an integer in the given base, 2 <= base <= 36,
// accepting an optional sign and, above base 10, letters of either case. ok
// is false if base is out of range, s is empty or not a valid number, or the
// value does not fit in an int64. Unlike Go, base 0 (prefix detection) and
// underscores are not supported.
func ParseInt64(s string, base int) (n int64, ok bool) {
	if base < 2 || base > 36 || len(s) == 0 {
		return 0, false
	}
	neg := false
	if s[0] == '-' || s[0] == '+' {
		neg = s[0] == '-'
		s = s[1:]
		if len(s) == 0 {
			return 0, false
		}
	}
	const min = -1 << 63
	b := int64(base)
	// Accumulate negatively: -min does not fit, min does.
	var acc int64
	for i := 0; i < len(s); i++ {
		c := s[i]
		var d int64
		if c >= '0' && c <= '9' {
			d = int64(c - '0')
		} else if c >= 'a' && c <= 'z' {
			d = int64(c-'a') + 10
		} else if c >= 'A' && c <= 'Z' {
			d = int64(c-'A') + 10
		} else {
			return 0, false
		}
		if d >= b || acc < min/b {
			return 0, false
		}
		acc *= b
		if acc < min+d {
			return 0, false
		}
		acc -= d
	}
	if !neg {
		if acc == min {
			return 0, false
		}
		acc = -acc
	}
	return acc, true
}
