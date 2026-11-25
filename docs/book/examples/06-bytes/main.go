// bytes.Buffer builds data; Freeze turns it into an immutable bytes.Bytes.
// A frozen Bytes over 64 bytes lives outside the actor heap and is shared,
// not copied, when sent.
package main

import (
	"gogo/actor"
	"gogo/bytes"
)

type Frame struct {
	Seq  int
	Data bytes.Bytes
}

func main() {
	buf := bytes.NewBuffer(256)
	for i := 0; i < 200; i++ {
		buf.WriteByte(byte('a' + i%26))
	}
	buf.WriteString("!")
	frame := buf.Freeze() // buf is empty again and can be reused
	println("frozen", frame.Len(), "bytes; buffer now", buf.Len())
	println("first", frame.Slice(0, 5).String(), "last", frame.At(frame.Len()-1))

	me := actor.Self()
	sink := actor.Spawn(func() {
		switch m := actor.Receive().(type) {
		case Frame:
			// The receiver holds the same 201 bytes, charged to it too.
			println("sink got frame", m.Seq, m.Data.Len(), m.Data.Slice(26, 29).String())
			actor.Send(me, actor.Info(actor.Self()).Memory > 201)
		}
	})
	actor.Send(sink, Frame{1, frame})
	println("sink charged for it:", actor.Receive())

	// Small helpers: from strings and slices, compare, copy out.
	a := bytes.FromString("ping")
	b := bytes.FromSlice([]byte{'p', 'i', 'n', 'g'})
	println(a.Equal(b), a.String())
	out := make([]byte, 2)
	println(a.CopyTo(out), string(out))
}
