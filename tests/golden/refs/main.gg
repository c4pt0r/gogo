package main

import "gogo/actor"

type Holder struct {
	Name string
	R    actor.Ref
}

func main() {
	r1, r2 := actor.MakeRef(), actor.MakeRef()
	println(r1 == r1, r1 == r2)
	println(r1 != r2, r2 != r2)
	println(r1, r2)

	actor.Send(actor.Self(), r1)
	switch got := actor.Receive().(type) {
	case actor.Ref:
		println(got == r1)
	default:
		println("not a ref")
	}

	actor.Send(actor.Self(), Holder{R: r2})
	switch m := actor.Receive().(type) {
	case Holder:
		println(m.R == r2, m.R == r1, m.Name, "|")
	}

	var reason actor.Reason
	println(reason, reason.Code, actor.Killed, actor.Shutdown)
}
