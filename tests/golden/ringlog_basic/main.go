// gogo/ringlog: rotation, bounded size, Read of the newest bytes in order.
package main

import (
	"gogo/fs"
	"gogo/ringlog"
)

func size(p string) int {
	i, e := fs.Stat(p)
	if e != fs.OK {
		return -1
	}
	return i.Size
}

func main() {
	b, e := ringlog.Read("/flash/log", 100)
	println("empty", b.Len(), int(e))

	// 7-byte lines "line X\n", limit 64: rotate when 0.log > 32 bytes.
	max := 64
	worst := 0
	for i := 0; i < 20; i++ {
		l := "line " + "ABCDEFGHIJKLMNOPQRST"[i:i+1] + "\n"
		e := ringlog.Append("/flash/log", max, l)
		if e != fs.OK {
			println("append error", i, int(e))
		}
		t := size("/flash/log/0.log")
		if s := size("/flash/log/1.log"); s > 0 {
			t += s
		}
		if t > worst {
			worst = t
		}
		if i == 2 || i == 6 || i == 19 {
			println("after", i, size("/flash/log/0.log"), size("/flash/log/1.log"))
		}
	}
	println("bounded", worst <= max+len("line A\n"))

	b, e = ringlog.Read("/flash/log", 1000)
	println("all", b.Len(), int(e))
	println(b.String())
	b, e = ringlog.Read("/flash/log", 20)
	println("tail20", b.Len(), int(e))
	println(b.String())
	b, e = ringlog.Read("/flash/log", 3)
	println("tail3", b.Len(), int(e))
	println(b.String())
	b, e = ringlog.Read("/flash/log", 0)
	println("zero", b.Len(), int(e))

	// a different dir, bytes line, and a bad dir
	println("bytes", int(ringlog.Append("/flash/log2", 1000, []byte("xy\n"))))
	b, _ = ringlog.Read("/flash/log2", 100)
	println(b.String())
	println("bad", int(ringlog.Append("/flash/a b", 100, "x")))

	// A second writer: 0.log is held open, so Append is Busy and writes nothing.
	ringlog.Append("/flash/log3", 1000, "a\n")
	w, _ := fs.Open("/flash/log3/0.log", fs.Append)
	println("busy writer", int(ringlog.Append("/flash/log3", 1000, "b\n")))
	w.Close()
	println("size", size("/flash/log3/0.log"))

	// A reader holds 1.log: the rotation fails with Busy, the line is written.
	ringlog.Append("/flash/log4", 4, "xxxx\n") // rotates: 1.log exists
	r, _ := fs.Open("/flash/log4/1.log", fs.Read)
	println("busy rotate", int(ringlog.Append("/flash/log4", 4, "yyyy\n")), size("/flash/log4/0.log"))
	r.Close()
	println("rotate retried", int(ringlog.Append("/flash/log4", 4, "z\n")), size("/flash/log4/0.log"), size("/flash/log4/1.log"))
}
