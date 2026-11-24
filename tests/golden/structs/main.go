package main

type P struct{ X, Y int }

type L struct {
	A    P
	Name string
}

func main() {
	l := L{P{1, 2}, "a"}
	println(l.A.X+l.A.Y, l) // 3 {{1 2} a}
}
