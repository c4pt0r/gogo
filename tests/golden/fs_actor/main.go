// gogo/fs and actors: a crashed or killed owner's Write file is committed
// and closed (slot, charge and Busy are gone at once), Handoff moves the
// file to the new owner, and a stale or foreign File is Closed.
package main

import (
	"gogo/actor"
	"gogo/fs"
	"gogo/port"
)

type Opened struct{ F fs.File }

type Go struct{}

func content(path string) string {
	b, e := fs.ReadFile(path, 100)
	if e != fs.OK {
		return "error"
	}
	return b.String()
}

// waitDown waits for the death of the monitored actor.
func waitDown() {
	switch actor.Receive().(type) {
	case actor.DownMsg:
	}
}

func main() {
	me := actor.Self()

	// 1. An owner that crashes while it holds a Write file.
	crasher := actor.Spawn(func() {
		f, e := fs.Open("/flash/crash.txt", fs.Write)
		println("crasher open", int(e))
		println("crasher write", int(f.Write("written before the crash")))
		zero := 0
		println(1 / zero)
	})
	actor.Monitor(crasher)
	waitDown()
	println("after crash:", content("/flash/crash.txt"))
	f, e := fs.Open("/flash/crash.txt", fs.Write) // no stale Busy
	println("reopen for write", int(e))
	f.Write("second")
	f.Close()
	println("rewritten:", content("/flash/crash.txt"))

	// 2. The slots are freed: all four can be opened again, and a fifth is TooMany.
	for i := 0; i < 3; i++ {
		c := actor.Spawn(func() {
			for j := 0; j < 4; j++ {
				_, _ = fs.Open("/flash/crash.txt", fs.Read)
			}
			zero := 0
			println(1 / zero)
		})
		actor.Monitor(c)
		waitDown()
	}
	a, e1 := fs.Open("/flash/crash.txt", fs.Read)
	b, e2 := fs.Open("/flash/crash.txt", fs.Read)
	c, e3 := fs.Open("/flash/crash.txt", fs.Read)
	d, e4 := fs.Open("/flash/crash.txt", fs.Read)
	_, e5 := fs.Open("/flash/crash.txt", fs.Read)
	println("four after crashes", int(e1), int(e2), int(e3), int(e4), "fifth", int(e5))
	a.Close()
	b.Close()
	c.Close()
	d.Close()

	// 3. An owner that is killed while holding a Write file.
	sleeper := actor.Spawn(func() {
		f, _ := fs.Open("/flash/killed.txt", fs.Write)
		f.Write("kept")
		actor.Send(me, Opened{f})
		actor.Receive()
	})
	var held fs.File
	switch m := actor.Receive().(type) {
	case Opened:
		held = m.F
	}
	actor.Monitor(sleeper)
	_, e = fs.Open("/flash/killed.txt", fs.Write)
	println("while held", int(e))
	actor.Kill(sleeper, 9)
	waitDown()
	println("after kill:", content("/flash/killed.txt"))
	_, e = held.Read(1)
	println("stale file", int(e))
	println("stale write", int(held.Write("x")))
	held.Close()
	w, e := fs.Open("/flash/killed.txt", fs.Write)
	println("reopen after kill", int(e))
	w.Close()

	// 4. Handoff: the new owner writes and its exit commits the file.
	hf, e := fs.Open("/flash/handoff.txt", fs.Write)
	hf.Write("parent;")
	heir := actor.Spawn(func() {
		actor.Receive()
		actor.Receive()
	})
	actor.Monitor(heir)
	println("handoff", port.Handoff(actor.PID(hf), heir))
	println("old owner write", int(hf.Write("lost")))
	_, e = hf.Size()
	println("old owner size", int(e))
	println("old owner sync", int(hf.Sync()))
	actor.Send(heir, Opened{hf})
	actor.Send(heir, Go{})
	waitDown()
	println("after heir exit:", content("/flash/handoff.txt"))
	println("busy gone", int(fs.Remove("/flash/handoff.txt")))

	// A File handed to a live actor is written by that actor only.
	worker := actor.Spawn(func() {
		switch m := actor.Receive().(type) {
		case Opened:
			println("heir write", int(m.F.Write("heir;")))
			n, e := m.F.Size()
			println("heir size", n, int(e))
		}
		actor.Send(me, Go{})
		actor.Receive()
	})
	wf, _ := fs.Open("/flash/h2.txt", fs.Write)
	port.Handoff(actor.PID(wf), worker)
	actor.Send(worker, Opened{wf})
	actor.Receive() // the worker is done with the file
	println("owner now the worker:", int(wf.Write("x")))
	wf.Close() // anyone holding the PID may close it
	println("after close:", content("/flash/h2.txt"))

	// 5. A PID that is no file, and a closed one.
	nf := fs.File(actor.Self())
	_, e = nf.Read(1)
	println("non-file read", int(e))
	println("non-file write", int(nf.Write("x")))
	nf.Close()
	println("still alive")
}
