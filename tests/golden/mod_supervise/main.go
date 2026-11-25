// SuperviseModule and Upgrade: a module worker is restarted from its module
// after a crash, then hot-swapped to v2; the v1 module unloads.
package main

import (
	"gogo/actor"
	"gogo/code"
	"gogo/fs"
	"gogo/supervisor"
)

func load(path string) code.Module {
	b, e := fs.ReadFile(path, 100000)
	if e != fs.OK {
		println("read", path, int(e))
	}
	m, le := code.Load(b)
	if le != code.OK {
		println("load", path, int(le))
	}
	return m
}

// next returns the PID the (re)started worker announces.
func next() actor.PID {
	var p actor.PID
	switch x := actor.Receive().(type) {
	case actor.PID:
		p = x
	}
	return p
}

func main() {
	v1 := load("/flash/worker_v1.gmod")
	size1 := code.MemUsed()
	v2 := load("/flash/worker_v2.gmod")
	size2 := code.MemUsed() - size1
	println("loaded both", size1 > 0, size2 > 0)
	sup := supervisor.SuperviseModule(v1, "Work", actor.Self(),
		supervisor.Spec{Restart: supervisor.Permanent, MaxRestarts: 5})
	// The supervisor already holds v1: releasing right away is safe.
	code.Release(v1)
	w := next()
	actor.Send(w, 1)
	actor.Send(w, 2) // crashes v1
	w = next()       // restarted from the module, not NotFound
	println("restarted", w != sup)
	actor.Send(w, 3)
	actor.Sleep(50)
	println("code mem before upgrade", code.MemUsed() == size1+size2)

	// An upgrade that fails (no entry "Work") restarts the child from v1,
	// which stays loaded and held.
	bad := load("/flash/worker_bad.gmod")
	r, _ := actor.Call(sup, supervisor.Upgrade{Module: bad}, 2000)
	println("bad upgrade", r)
	code.Release(bad)
	w = next()
	actor.Send(w, 5)
	actor.Sleep(50)
	println("code mem after failed upgrade", code.MemUsed() == size1+size2)
	r, _ = actor.Call(sup, supervisor.Upgrade{Module: v1}, 2000)
	println("same module", r)

	r, _ = actor.Call(sup, supervisor.Upgrade{Module: v2}, 2000)
	println("upgrade", r)
	w = next()
	actor.Send(w, 4)
	actor.Sleep(50)
	code.Release(v2)
	println("code mem after upgrade", code.MemUsed() == size2)

	// A supervisor that cannot start: the module is gone, no PID.
	gone := load("/flash/worker_bad.gmod")
	code.Release(gone)
	none := supervisor.SuperviseModule(gone, "Other", 0, supervisor.Spec{})
	var zero actor.PID
	println("stale module", none == zero)
}
