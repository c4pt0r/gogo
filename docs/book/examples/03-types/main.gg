// The basic types of the subset: int (32 bits), int64, byte, bool, string,
// named basic types, constants and iota.
package main

// Weekday is a named int type. Its constants come from iota.
type Weekday int

const (
	Sunday Weekday = iota
	Monday
	Tuesday
)

const kilo = 1024

func main() {
	// int is always 32 bits and wraps around on overflow, as in Go.
	big := 2147483647
	big++
	println("int wraps:", big)

	// int64 for larger numbers; actor.Now() returns one too.
	var t int64 = 1 << 40
	println("int64:", t*3)

	// byte arithmetic wraps at 256.
	b := byte(250)
	b += 10
	println("byte:", b)

	println("bool:", 3 > 2, !true)
	println("string:", "gogo"+"!", len("gogo"))
	println("named:", Monday, Tuesday, int(Tuesday)*kilo)

	// Integer division truncates toward zero; % has the dividend's sign.
	println("div:", -7/2, -7%2, 7/2)

	// Conversions between the integer types are explicit.
	var n int = 300
	println("convert:", int64(n)*1000000, byte(n), int(int64(5)))
}
