// gogo/fs: each Err value reachable on the unix host (all but NoSpace and
// IO), with /sd absent (no sd/ in the fs root).
package main

import (
	"gogo/actor"
	"gogo/bytes"
	"gogo/fs"
)

func show(what string, e fs.Err) {
	println(what, int(e))
}

func open(path string, mode int) fs.File {
	f, e := fs.Open(path, mode)
	if e != fs.OK {
		println("unexpected open failure", path, int(e))
	}
	return f
}

func main() {
	// OK is 0 and the values are the documented numbers.
	println(int(fs.OK), int(fs.NotFound), int(fs.Exists), int(fs.NotMounted), int(fs.NoSpace), int(fs.TooMany), int(fs.Busy),
		int(fs.BadPath), int(fs.IsDir), int(fs.NotDir), int(fs.NotEmpty), int(fs.NoMemory), int(fs.IO), int(fs.Closed))

	// NotFound.
	_, e := fs.Open("/flash/missing", fs.Read)
	show("open missing", e)
	_, e = fs.ReadFile("/flash/missing", 10)
	show("readfile missing", e)
	_, e = fs.Stat("/flash/missing")
	show("stat missing", e)
	_, e = fs.ReadDir("/flash/missing")
	show("readdir missing", e)
	show("remove missing", fs.Remove("/flash/missing"))
	show("rename missing", fs.Rename("/flash/missing", "/flash/other"))
	show("mkdir under missing", fs.Mkdir("/flash/missing/sub"))

	// Exists.
	show("mkdir", fs.Mkdir("/flash/d"))
	show("mkdir again", fs.Mkdir("/flash/d"))
	show("write a", fs.WriteFile("/flash/a", "A"))
	show("write b", fs.WriteFile("/flash/b", "B"))
	show("rename onto file", fs.Rename("/flash/a", "/flash/b"))
	show("rename onto dir", fs.Rename("/flash/a", "/flash/d"))

	// NotMounted: /sd has no card here; unknown prefixes; a prefix is a whole segment.
	_, e = fs.Open("/sd/x", fs.Read)
	show("sd open", e)
	show("sd mkdir", fs.Mkdir("/sd/x"))
	show("sd writefile", fs.WriteFile("/sd/x", "x"))
	_, e = fs.Stat("/sd")
	show("sd stat root", e)
	_, e = fs.Usage("/sd")
	show("sd usage", e)
	_, e = fs.Stat("/flashx/a")
	show("flashx", e)
	_, e = fs.Stat("/tmp/a")
	show("unknown prefix", e)

	// TooMany: 4 open files at most.
	f0 := open("/flash/a", fs.Read)
	f1 := open("/flash/a", fs.Read)
	f2 := open("/flash/b", fs.Read)
	f3 := open("/flash/b", fs.Read)
	_, e = fs.Open("/flash/a", fs.Read)
	show("fifth open", e)
	f3.Close()
	g, e := fs.Open("/flash/a", fs.Read)
	show("after close", e)
	g.Close()

	f1.Close()
	f2.Close()

	// Busy: one writer per path, and no remove or rename of an open file.
	w := open("/flash/w", fs.Write)
	_, e = fs.Open("/flash/w", fs.Write)
	show("second writer", e)
	_, e = fs.Open("/flash/w", fs.Append)
	show("append beside writer", e)
	r, e := fs.Open("/flash/w", fs.Read)
	show("reader beside writer", e)
	r.Close()
	show("remove open", fs.Remove("/flash/w"))
	show("rename open source", fs.Rename("/flash/w", "/flash/w2"))
	show("rename onto open", fs.Rename("/flash/b", "/flash/w"))
	show("writefile over open", fs.WriteFile("/flash/a", "again"))
	w.Close()
	show("remove closed", fs.Remove("/flash/w"))
	f0.Close()
	show("writefile closed", fs.WriteFile("/flash/a", "again"))

	// BadPath.
	_, e = fs.Stat("flash/a")
	show("relative", e)
	_, e = fs.Stat("/flash/")
	show("trailing slash", e)
	_, e = fs.Stat("/flash//a")
	show("empty segment", e)
	_, e = fs.Stat("/flash/a/..")
	show("dot dot", e)
	_, e = fs.Stat("/flash/.")
	show("dot", e)
	_, e = fs.Stat("/flash/a b")
	show("space", e)
	_, e = fs.Stat("/flash/ab/c")
	show("fine name, absent dir", e)
	name33 := "/flash/123456789012345678901234567890123"
	_, e = fs.Stat(name33)
	show("33-byte name", e)
	_, e = fs.Stat(name33[:len(name33)-1])
	show("32-byte name", e)
	long := "/flash/12345678901234567890123456789012/12345678901234567890123456789012/12345678901234567890123456789012"
	_, e = fs.Stat(long[:96])
	show("96-byte path", e)
	_, e = fs.Stat(long[:97])
	show("97-byte path", e)
	_, e = fs.Stat("/")
	show("stat /", e)
	_, e = fs.ReadDir("")
	show("empty path", e)
	show("mkdir mount root", fs.Mkdir("/flash"))
	show("remove mount root", fs.Remove("/flash"))
	show("rename mount root", fs.Rename("/flash", "/flash/z"))
	show("writefile mount root", fs.WriteFile("/flash", "x"))
	_, e = fs.Open("/flash", fs.Write)
	show("open mount root", e)

	// IsDir and NotDir.
	_, e = fs.Open("/flash/d", fs.Write)
	show("open dir for write", e)
	_, e = fs.ReadFile("/flash/d", 10)
	show("readfile dir", e)
	show("writefile dir", fs.WriteFile("/flash/d", "x"))
	_, e = fs.Stat("/flash/a/x")
	show("through a file", e)
	show("mkdir through a file", fs.Mkdir("/flash/a/x"))
	_, e = fs.ReadDir("/flash/a")
	show("readdir of a file", e)

	// NotEmpty.
	show("write in d", fs.WriteFile("/flash/d/f", "f"))
	show("remove full dir", fs.Remove("/flash/d"))
	show("remove file", fs.Remove("/flash/d/f"))
	show("remove empty dir", fs.Remove("/flash/d"))

	// NoMemory: ReadFile over max.
	buf := bytes.NewBuffer(5000)
	for i := 0; i < 500; i++ {
		buf.WriteString("0123456789")
	}
	show("write big", fs.WriteFile("/flash/big", buf.Freeze()))
	_, e = fs.ReadFile("/flash/big", 4999)
	show("readfile over max", e)
	b, e := fs.ReadFile("/flash/big", 5000)
	show("readfile at max", e)
	println(b.Len())

	// Closed: use after close, wrong mode, zero File, another actor, a non-file PID.
	f := open("/flash/big", fs.Read)
	f.Close()
	_, e = f.Read(1)
	show("read after close", e)
	_, e = f.Size()
	show("size after close", e)
	show("sync after close", f.Sync())
	show("write after close", f.Write("x"))
	f.Close() // closing twice is harmless
	var zero fs.File
	_, e = zero.Read(1)
	show("zero file", e)
	wf := open("/flash/m", fs.Write)
	_, e = wf.Read(1)
	show("read a write file", e)
	rf := open("/flash/big", fs.Read)
	show("write a read file", rf.Write("x"))
	me := actor.Self()
	actor.Spawn(func() {
		_, e := rf.Read(1)
		show("read by non-owner", e)
		show("write by non-owner", wf.Write("x"))
		_, e = rf.Size()
		show("size by non-owner", e)
		actor.Send(me, true)
	})
	actor.Receive()
	b, e = rf.Read(1 << 30)
	show("read clamps", e)
	println(b.Len())
	rf.Close()
	wf.Close()
	show("sync closed", wf.Sync())
}
