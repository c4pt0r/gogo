// Storage errors are values, never crashes: every call returns an fs.Err.
// And a File is a port: it closes when its owner exits, even by a crash.
package main

import (
	"gogo/actor"
	"gogo/fs"
)

func main() {
	_, e := fs.ReadFile("/flash/missing.txt", 100)
	println("missing file:", int(e), e == fs.NotFound)
	println("bad name:", int(fs.WriteFile("/flash/bad name.txt", "x")))
	println("relative path:", int(fs.WriteFile("notes.txt", "x")))
	println("no such mount:", int(fs.WriteFile("/usb/a.txt", "x")))
	println("no card (/sd):", int(fs.WriteFile("/sd/a.txt", "x")))
	fs.Mkdir("/flash/d")
	fs.WriteFile("/flash/d/a", "x")
	println("remove non-empty dir:", int(fs.Remove("/flash/d")))

	// Too big for the caller's quota: NoMemory, and nothing is read.
	_, e = fs.ReadFile("/flash/d/a", 0)
	println("max 0:", int(e))

	// A writer that crashes still closes (and commits) its file.
	me := actor.Self()
	w := actor.Spawn(func() {
		f, _ := fs.Open("/flash/crash.txt", fs.Write)
		f.Write("written before the crash\n")
		actor.Send(me, "wrote")
		actor.Receive()
		zero := 0
		println(1 / zero)
	})
	println(actor.Receive())
	// While the writer holds the file open, removing it is Busy.
	println("remove while open:", int(fs.Remove("/flash/crash.txt")))
	ref := actor.Monitor(w)
	actor.Send(w, "crash now")
	switch m := actor.Receive().(type) {
	case actor.DownMsg:
		println("writer down:", m.Ref == ref)
	}
	b, e := fs.ReadFile("/flash/crash.txt", 100)
	println("after the crash:", int(e), b.Len(), "bytes")
	println("remove now:", int(fs.Remove("/flash/crash.txt")))
}
