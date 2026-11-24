// One 1 KB off-heap Bytes sent to 3 actors: each holds it and is charged
// its full size; after they exit, the sender's own memory is unchanged.
package main

import (
	"gogo/actor"
	"gogo/bytes"
)

type Mem struct{ N int }

func child(me actor.PID) {
	switch m := actor.Receive().(type) {
	case bytes.Bytes:
		actor.Send(me, Mem{actor.Info(actor.Self()).Memory})
		actor.Receive()
		println("child", m.Len(), m.At(1023))
	}
}

func main() {
	me := actor.Self()
	b := bytes.NewBuffer(1024)
	for i := 0; i < 1024; i++ {
		b.WriteByte(byte(i))
	}
	x := b.Freeze()
	pids := []actor.PID{actor.Spawn(func() { child(me) }), actor.Spawn(func() { child(me) }), actor.Spawn(func() { child(me) })}
	before := actor.Info(me).Memory
	for _, p := range pids {
		actor.Send(p, x)
	}
	for i := 0; i < 3; i++ {
		switch m := actor.Receive().(type) {
		case Mem:
			println("holder charged in full:", m.N >= 1024)
		}
	}
	for _, p := range pids {
		r := actor.Monitor(p)
		actor.Send(p, 0)
		actor.Receive()
		actor.Demonitor(r)
	}
	println("sender unchanged:", actor.Info(me).Memory == before, x.At(5))
}
