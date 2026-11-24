// A closure capturing a 3000-byte off-heap Bytes, spawned with a 2048-byte
// limit: the child is charged the Bytes in full, so it exits with
// OutOfMemory before running; the spawner is unaffected. With room, the
// child runs and its memory includes the Bytes.
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
		println("unreachable", x.Len())
	}, actor.Options{MaxMemory: 2048})
	r := actor.Monitor(small)
	switch m := actor.Receive().(type) {
	case actor.DownMsg:
		println("down", m.Ref == r, m.Reason.Code == actor.OutOfMemory)
	}
	actor.SpawnOpt(func() {
		println("ran", x.Len(), actor.Info(actor.Self()).Memory > 3000)
		actor.Send(me, 1)
	}, actor.Options{MaxMemory: 8192})
	actor.Receive()
	println("spawner", x.Len())
}
