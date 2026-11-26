// This program does NOT compile. Closures copy what they capture when they
// are made, so gogo rejects a captured variable that changes afterwards
// (in Go the closure would see the change; in gogo it could not).
package main

func main() {
	n := 0
	show := func() { println(n) }
	n = 5
	show()
	var b byte = 7
	var v any = b // a byte cannot go into any
	println(v)
}
