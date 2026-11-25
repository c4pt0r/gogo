// /sd works like /flash, on boards with a card slot (the Cardputer ADV).
// Copy a file from /flash to /sd: Rename cannot cross mounts, so read and
// write. On the host, --fs-root DIR gives /sd only if DIR/sd exists.
package main

import "gogo/fs"

func copyFile(from, to string) fs.Err {
	b, e := fs.ReadFile(from, 4096)
	if e != fs.OK {
		return e
	}
	return fs.WriteFile(to, b)
}

func main() {
	fs.WriteFile("/flash/settings.txt", "volume=3\n")
	println("rename across mounts:", int(fs.Rename("/flash/settings.txt", "/sd/settings.txt")) == int(fs.BadPath))
	e := copyFile("/flash/settings.txt", "/sd/settings.txt")
	if e == fs.NotMounted {
		println("no SD card")
		return
	}
	b, _ := fs.ReadFile("/sd/settings.txt", 100)
	println("copied:", b.Len(), "bytes")
	sp, e := fs.Usage("/sd")
	println("usage ok:", e == fs.OK, sp.Total > 0, sp.Used <= sp.Total)
}
