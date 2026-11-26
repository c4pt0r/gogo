package main

import "gogo/actor"

type Item struct{ N int }

type Done struct{}

func main() {
	// The receiver never receives: its mailbox fills up to its 4096-byte
	// limit, it exits with MailboxOverflow and later messages are dropped.
	// The sender is not affected.
	sink := actor.SpawnOpt(func() {
		n := 0
		for {
			n++
		}
	}, actor.Options{MaxMemory: 4096})
	for i := 0; i < 2000; i++ {
		actor.Send(sink, Item{i})
	}
	me := actor.Self()
	actor.Spawn(func() {
		actor.Send(me, Done{})
	})
	switch actor.Receive().(type) {
	case Done:
	}
	println("sender alive")
}
