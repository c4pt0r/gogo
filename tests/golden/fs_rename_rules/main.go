// gogo/fs path and rename rules: a name ending in "." is BadPath on every
// mount (FAT would strip the dot and reach another file); renaming into its
// own subtree is BadPath (onto itself stays Exists); renaming a directory
// while any file is open on that mount is Busy (a plain file still renames).
package main

import "gogo/fs"

func show(what string, e fs.Err) {
	println(what, int(e))
}

func main() {
	show("write a.txt", fs.WriteFile("/flash/a.txt", "A"))
	f, e := fs.Open("/flash/a.txt", fs.Write)
	show("open a.txt", e)
	_, e = fs.Open("/flash/a.txt.", fs.Write)
	show("open a.txt.", e)
	_, e = fs.Stat("/sd/a.txt.")
	show("sd stat a.txt.", e)
	show("remove a.txt..", fs.Remove("/flash/a.txt.."))
	show("mkdir d.", fs.Mkdir("/flash/d."))
	_, e = fs.Stat("/flash/d./x")
	show("dot segment", e)
	_, e = fs.Stat("/flash/.hidden")
	show("leading dot", e)
	f.Close()

	show("mkdir p", fs.Mkdir("/flash/p"))
	show("rename p into p/q", fs.Rename("/flash/p", "/flash/p/q"))
	show("rename p onto p", fs.Rename("/flash/p", "/flash/p"))
	show("rename a.txt onto a.txt", fs.Rename("/flash/a.txt", "/flash/a.txt"))
	show("sd mkdir d", fs.Mkdir("/sd/d"))
	show("sd rename d into d/x", fs.Rename("/sd/d", "/sd/d/x"))
	show("rename p to pq", fs.Rename("/flash/p", "/flash/pq"))

	show("mkdir d", fs.Mkdir("/flash/d"))
	w, e := fs.Open("/flash/d/log", fs.Write)
	show("open d/log", e)
	show("rename d (file open inside)", fs.Rename("/flash/d", "/flash/e"))
	show("rename pq (file open elsewhere)", fs.Rename("/flash/pq", "/flash/p"))
	show("rename a file", fs.Rename("/flash/a.txt", "/flash/b.txt"))
	show("sd rename d", fs.Rename("/sd/d", "/sd/e"))
	w.Close()
	show("rename d after close", fs.Rename("/flash/d", "/flash/e"))
	_, e = fs.Stat("/flash/e/log")
	show("stat e/log", e)
}
