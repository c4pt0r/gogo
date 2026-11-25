// Send copies the whole message, including slices and nested structs. The
// receiver's copy keeps the sharing inside the message (two views of one
// slice stay one slice), but nothing is shared between the two actors.
package main

import "gogo/actor"

type Batch struct {
	Name  string
	All   []int
	First []int // a view into All
}

func main() {
	me := actor.Self()
	nums := []int{1, 2, 3, 4}
	b := Batch{Name: "b1", All: nums, First: nums[:2]}

	w := actor.Spawn(func() {
		switch m := actor.Receive().(type) {
		case Batch:
			m.First[0] = 99 // First and All share one array in the copy too
			println("receiver:", m.All[0], m.First[0])
			actor.Send(me, true)
		}
	})
	actor.Send(w, b)
	nums[1] = -2 // after the send: the receiver never sees this
	actor.Receive()
	println("sender:", nums[0], nums[1], b.First[1])
}
