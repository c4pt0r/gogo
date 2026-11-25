// A boot counter in gogo/kv: read, increment, store. Each run of the
// program (each boot, on a board) adds one. The example counts three
// "boots" in one run by calling boot three times.
package main

import (
	"gogo/fs"
	"gogo/kv"
	"gogo/strconv"
)

func boot() int {
	n := 0
	v, e := kv.Get("boots")
	if e == fs.OK {
		n, _ = strconv.Atoi(v.String())
	} else if e != fs.NotFound {
		println("cannot read the counter, error", int(e))
		return -1
	}
	n++
	if e := kv.Put("boots", strconv.Itoa(n)); e != fs.OK {
		println("cannot store the counter, error", int(e))
	}
	return n
}

func main() {
	for i := 0; i < 3; i++ {
		println("boot number", boot())
	}
	kv.Put("owner", "dev board 7")
	keys, _ := kv.Keys()
	println("keys stored:", len(keys))
	v, _ := kv.Get("owner")
	println("owner:", v.String())
	println("delete:", int(kv.Delete("owner")), "again:", int(kv.Delete("owner")))
	println("bad key:", kv.Put("a/b", "x") == fs.BadPath)
}
