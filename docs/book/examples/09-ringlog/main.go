// gogo/ringlog keeps a log on flash that never grows past a size bound:
// two files, the older one dropped when the newer one is half full.
package main

import (
	"gogo/fs"
	"gogo/ringlog"
	"gogo/strconv"
)

func main() {
	for i := 1; i <= 30; i++ {
		e := ringlog.Append("/flash/events", 100, "event "+strconv.Itoa(i)+"\n")
		if e != fs.OK {
			println("append", i, "error", int(e))
		}
	}
	// Read returns the newest bytes, oldest first.
	b, _ := ringlog.Read("/flash/events", 1000)
	println("kept", b.Len(), "bytes:")
	println(b.String())
	tail, _ := ringlog.Read("/flash/events", 9)
	println("last 9 bytes: [" + tail.String() + "]")
}
