// bytes.Bytes: construction, the read methods, sharing slices of a large
// (off-heap) Bytes, type switches and messages carrying Bytes in structs.
package main

import (
	"gogo/actor"
	"gogo/bytes"
)

type Frame struct {
	Seq  int
	Body bytes.Bytes
}

func big(n int) bytes.Bytes {
	b := bytes.NewBuffer(n)
	for i := 0; i < n; i++ {
		b.WriteByte(byte('a' + i%26))
	}
	return b.Freeze()
}

func at(x bytes.Bytes, i int) byte { return x.At(i) }

func main() {
	var zero bytes.Bytes
	println(zero.Len(), zero.String() == "", zero.Equal(bytes.FromString("")))

	x := bytes.FromString("hello, world")
	p := []byte("hello")
	y := bytes.FromSlice(p)
	p[0] = 'J' // y is a copy
	println(x.Len(), x.At(0), x.String(), y.String())
	println(x.Slice(0, 5).Equal(y), x.Slice(7, 12).String(), x.Slice(3, 3).Len())
	q := make([]byte, 4)
	println(x.CopyTo(q), string(q), y.CopyTo(q[1:]), string(q))
	println(string([]byte(x.Slice(7, 9).String())))

	// A large Bytes and a large slice of it (shared), a small one (copied).
	l := big(300)
	mid := l.Slice(26, 226)
	println(l.Len(), mid.Len(), mid.At(0), mid.At(199), mid.Slice(0, 3).String())
	println(actor.Info(actor.Self()).Memory >= 300)

	// Type switch on any, and a struct carrying Bytes through a message.
	me := actor.Self()
	pid := actor.Spawn(func() {
		for i := 0; i < 3; i++ {
			switch m := actor.Receive().(type) {
			case bytes.Bytes:
				println("bytes", m.Len(), m.Slice(0, 3).String())
			case Frame:
				println("frame", m.Seq, m.Body.Len(), m.Body.At(m.Body.Len()-1))
			case string:
				println("string", m)
			}
		}
		actor.Send(me, 1)
	})
	actor.Send(pid, "hi")
	actor.Send(pid, mid)
	actor.Send(pid, Frame{Seq: 7, Body: l})
	actor.Receive()

	// A spawned closure capturing a large Bytes holds it too.
	actor.Spawn(func() {
		println("captured", l.Len(), l.At(299), actor.Info(actor.Self()).Memory >= 300)
		actor.Send(me, 2)
	})
	actor.Receive()

	// Out of range: badcode.
	actor.Spawn(func() { println(at(x, 12)) })
	actor.Sleep(20)
	println("done")
}
