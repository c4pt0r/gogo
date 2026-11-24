package main

import "gogo/actor"

// Two overlapping views of one array, sent in one message.
type Views struct {
	Head []int
	Mid  []int
	Name []byte
}

type Done struct{}

func main() {
	me := actor.Self()
	arr := []int{0, 1, 2, 3, 4, 5}
	v := Views{Head: arr[:4], Mid: arr[2:5], Name: []byte("ab")}

	pid := actor.Spawn(func() {
		switch m := actor.Receive().(type) {
		case Views:
			// The copy keeps the sharing: Head[3] and Mid[1] are one element.
			m.Head[3] = 33
			println("recv", m.Mid[1], len(m.Head), len(m.Mid), cap(m.Mid))
			// The whole backing array came along: reslice Mid up to its cap.
			full := m.Mid[:cap(m.Mid)]
			println("tail", full[len(full)-1])
			// The sender's writes after the send are not seen.
			actor.Send(me, Done{})
			actor.Receive()
			println("after", m.Head[0], m.Mid[0], string(m.Name))
			actor.Send(me, Done{})
		}
	})
	actor.Send(pid, v)
	actor.Receive()
	arr[0] = 100
	arr[2] = 200
	v.Name[0] = 'X'
	println("sender", arr[3], v.Mid[1], string(v.Name))
	actor.Send(pid, Done{})
	actor.Receive()
}
