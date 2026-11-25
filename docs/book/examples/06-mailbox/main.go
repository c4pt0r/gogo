// Queued messages count against the RECEIVER's memory limit. A receiver
// that falls behind exits with MailboxOverflow; the sender is never
// slowed down or killed.
package main

import "gogo/actor"

type Sample struct{ N, Value int }

func main() {
	slow := actor.SpawnOpt(func() {
		actor.Sleep(1000) // never reads in time
	}, actor.Options{MaxMemory: 4096})
	actor.Monitor(slow)
	for i := 0; i < 1000; i++ {
		actor.Send(slow, Sample{i, i * i})
	}
	println("sender sent 1000 messages")
	switch m := actor.Receive().(type) {
	case actor.DownMsg:
		println("receiver exited, MailboxOverflow:", m.Reason.Code == actor.MailboxOverflow)
	}
}
