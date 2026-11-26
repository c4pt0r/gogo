// Tries code.Load on bytes that are no image.
package main

import (
	"gogo/bytes"
	"gogo/code"
)

func main() {
	_, e := code.Load(bytes.FromString("abcdefgh"))
	println("load", int(e))
}
