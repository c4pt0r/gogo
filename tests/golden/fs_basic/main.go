// gogo/fs: every function and method, every open mode, a listing with a
// subdirectory, Usage and Stat of the mount root.
package main

import (
	"gogo/bytes"
	"gogo/fs"
)

func ok(what string, e fs.Err) {
	println(what, int(e))
}

func content(path string) string {
	b, e := fs.ReadFile(path, 1000)
	if e != fs.OK {
		return "error"
	}
	return b.String()
}

func sorted(names []string) []string {
	for i := 1; i < len(names); i++ {
		for j := i; j > 0 && names[j] < names[j-1]; j-- {
			names[j], names[j-1] = names[j-1], names[j]
		}
	}
	return names
}

func main() {
	root, e := fs.Stat("/flash")
	println("root", root.Size, root.IsDir, int(e))

	ok("mkdir", fs.Mkdir("/flash/d"))
	d, e := fs.Stat("/flash/d")
	println("d", d.Size, d.IsDir, int(e))

	ok("writefile", fs.WriteFile("/flash/a.txt", "hello"))
	println("a.txt", content("/flash/a.txt"))
	ok("writefile replace", fs.WriteFile("/flash/a.txt", bytes.FromString("hello again")))
	println("a.txt", content("/flash/a.txt"))
	ok("writefile slice", fs.WriteFile("/flash/s.txt", []byte{'s', 'l', 'i', 'c', 'e'}))
	println("s.txt", content("/flash/s.txt"))

	// Write mode: string, Bytes and []byte, Sync, Size.
	f, e := fs.Open("/flash/b.txt", fs.Write)
	println("open write", int(e))
	ok("write string", f.Write("abc"))
	ok("write bytes", f.Write(bytes.FromString("def")))
	ok("write slice", f.Write([]byte{'g', 'h'}))
	ok("sync", f.Sync())
	n, e := f.Size()
	println("size", n, int(e))
	f.Close()
	info, e := fs.Stat("/flash/b.txt")
	println("b.txt", info.Size, info.IsDir, int(e))

	// Append mode.
	f, e = fs.Open("/flash/b.txt", fs.Append)
	println("open append", int(e))
	ok("append", f.Write("XY"))
	n, e = f.Size()
	println("size", n, int(e))
	f.Close()
	println("b.txt", content("/flash/b.txt"))

	// Read mode: successive reads, end of file, Read(0), Size.
	f, e = fs.Open("/flash/b.txt", fs.Read)
	println("open read", int(e))
	p, e := f.Read(4)
	println("read", p.String(), p.Len(), int(e))
	p, e = f.Read(100)
	println("read", p.String(), p.Len(), int(e))
	p, e = f.Read(10)
	println("eof", p.Len(), int(e))
	p, e = f.Read(0)
	println("read0", p.Len(), int(e))
	n, e = f.Size()
	println("size", n, int(e))
	f.Close()

	// Write truncates.
	f, _ = fs.Open("/flash/b.txt", fs.Write)
	f.Close()
	info, _ = fs.Stat("/flash/b.txt")
	println("truncated", info.Size)

	// Rename, then the listing (directories end in "/").
	ok("rename", fs.Rename("/flash/b.txt", "/flash/c.txt"))
	_, e = fs.Stat("/flash/b.txt")
	println("old name", int(e))
	names, e := fs.ReadDir("/flash")
	println("readdir", len(names), int(e))
	for _, nm := range sorted(names) {
		println(" ", nm)
	}
	sub, e := fs.ReadDir("/flash/d")
	println("empty dir", len(sub), int(e))

	// Usage.
	sp, e := fs.Usage("/flash")
	println("usage", sp.Total > 0, sp.Used <= sp.Total, int(e))

	// Remove files and the directory.
	ok("remove c", fs.Remove("/flash/c.txt"))
	ok("remove a", fs.Remove("/flash/a.txt"))
	ok("remove s", fs.Remove("/flash/s.txt"))
	ok("remove d", fs.Remove("/flash/d"))
	names, _ = fs.ReadDir("/flash")
	println("left", len(names))
}
