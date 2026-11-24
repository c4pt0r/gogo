// A 3000-byte off-heap Bytes sent to an actor whose limit is 2048 bytes:
// queued, it is charged to that actor's mailbox in full, which overflows
// it (mailbox_overflow); the sender is unaffected and keeps its Bytes.
package main

import (
	"gogo/actor"
	"gogo/bytes"
)

func main() {
	me := actor.Self()
	b := bytes.NewBuffer(0)
	for i := 0; i < 3000; i++ {
		b.WriteByte('x')
	}
	x := b.Freeze()
	small := actor.SpawnOpt(func() {
		actor.Receive()
		println("unreachable")
	}, actor.Options{MaxMemory: 2048})
	r := actor.Monitor(small)
	actor.Send(small, x)
	switch m := actor.Receive().(type) {
	case actor.DownMsg:
		println("down", m.Ref == r, m.Reason.Code == actor.MailboxOverflow)
	}
	// A bigger limit takes it.
	ok := actor.SpawnOpt(func() {
		switch m := actor.Receive().(type) {
		case bytes.Bytes:
			println("got", m.Len())
		}
		actor.Send(me, 1)
	}, actor.Options{MaxMemory: 8192})
	actor.Send(ok, x)
	actor.Receive()
	println("sender", x.Len())
}
