package main

import "gogo/port"

func main() {
	_, _ = port.Open("kbd", 1)
	println("not reached")
}
