// The type switch is how an actor tells messages apart. Cases can be
// struct types, int, int64, bool, string, actor.PID, actor.Ref and
// bytes.Bytes; default catches the rest.
package main

import "gogo/actor"

type Celsius struct{ Deg int }

type Reset struct{}

func describe(m any) {
	switch v := m.(type) {
	case Celsius:
		println("temperature", v.Deg)
	case Reset:
		println("reset")
	case int:
		println("int", v)
	case int64:
		println("int64", v)
	case string:
		println("string", v)
	case bool:
		println("bool", v)
	case actor.PID:
		println("a pid")
	default:
		println("something else")
	}
}

func main() {
	me := actor.Self()
	for _, m := range []any{Celsius{21}, Reset{}, 7, int64(7), "hi", true, me, []int{1}} {
		describe(m)
	}
}
