// A file whose name breaks the rules (made on a PC, say) is listed by
// ReadDir under its original name; opening it, or anything else with it in
// the path, is BadPath.
package main

import "gogo/fs"

func main() {
	names, e := fs.ReadDir("/flash")
	println("readdir", len(names), int(e))
	for _, n := range names {
		if n == "My File.TXT" {
			println("listed", n)
		}
		if n == "ok.txt" {
			println("listed", n)
		}
	}
	_, e = fs.Open("/flash/My File.TXT", fs.Read)
	println("open", int(e))
	_, e = fs.ReadFile("/flash/My File.TXT", 10)
	println("readfile", int(e))
	_, e = fs.Stat("/flash/My File.TXT")
	println("stat", int(e))
	println("remove", int(fs.Remove("/flash/My File.TXT")))
	b, e := fs.ReadFile("/flash/ok.txt", 10)
	println("ok.txt", b.String(), int(e))
}
