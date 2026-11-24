// bytes.Buffer: a reference type; writes of every kind, Len, Freeze (small
// and large), reuse after Freeze, and the unusable zero Buffer.
package main

import (
	"gogo/actor"
	"gogo/bytes"
)

type holder struct{ B bytes.Buffer }

func fill(b bytes.Buffer, n int) {
	for i := 0; i < n; i++ {
		b.WriteByte(byte('0' + i%10))
	}
}

func main() {
	b := bytes.NewBuffer(4)
	b.WriteString("GET ")
	b.Write([]byte("/index"))
	b.WriteByte('.')
	b.WriteBytes(bytes.FromString("html"))
	alias := b // the same buffer
	alias.WriteString(" HTTP/1.1")
	println(b.Len(), alias.Len())
	line := b.Freeze()
	println(line.String(), line.Len(), b.Len())

	// Reuse after Freeze; earlier results are unaffected.
	fill(b, 100)
	large := b.Freeze()
	b.WriteString("again")
	println(large.Len(), large.Slice(95, 100).String(), b.Freeze().String(), line.String())
	println(b.Freeze().Len())

	// A Buffer can be sent; the receiver gets its own copy.
	me := actor.Self()
	c := bytes.NewBuffer(0)
	c.WriteString("abc")
	actor.Spawn(func() {
		c.WriteString("def")
		println("child", c.Freeze().String())
		actor.Send(me, 1)
	})
	actor.Receive()
	println("parent", c.Freeze().String())

	// The zero Buffer is unusable: the checker rejects the forms it can
	// see (var without a value, bytes.Buffer{}, a literal leaving a Buffer
	// field out); one in a zero struct variable fails at run time.
	actor.Spawn(func() {
		var h holder
		h.B.WriteString("x")
	})
	actor.Sleep(20)
	println("done")
}
