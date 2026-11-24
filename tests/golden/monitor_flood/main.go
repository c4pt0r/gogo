package main

import "gogo/actor"

type Go struct{}

func main() {
	target := actor.Spawn(func() {
		actor.Receive()
	})
	// An actor that monitors the same target over and over without
	// Demonitor. Each monitor is charged to the watcher, so it hits its
	// own 4096-byte limit and crashes with oom; nothing else is affected.
	flooder := actor.SpawnOpt(func() {
		for {
			actor.Monitor(target)
		}
	}, actor.Options{MaxMemory: 4096})
	r := actor.Monitor(flooder)
	switch m := actor.Receive().(type) {
	case actor.DownMsg:
		println("flooder down", m.Ref == r, m.Reason.Code == actor.OutOfMemory)
	}
	actor.Send(target, Go{})
	println("main ok")
}
