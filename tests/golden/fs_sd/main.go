// The /sd mount (present when the runner creates sd/): writes there, usage,
// and a rename across the two media is BadPath.
package main

import "gogo/fs"

func main() {
	println("write", int(fs.WriteFile("/sd/note.txt", "on the card")))
	b, e := fs.ReadFile("/sd/note.txt", 100)
	println("read", b.String(), int(e))
	println("flash has none", int(fs.Remove("/flash/note.txt")))
	println("cross rename", int(fs.Rename("/sd/note.txt", "/flash/note.txt")))
	sp, e := fs.Usage("/sd")
	println("usage", sp.Total > 0, int(e))
	names, e := fs.ReadDir("/sd")
	println("dir", len(names), names[0], int(e))
}
