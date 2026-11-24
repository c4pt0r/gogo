// Console cost benchmark: 1000 println lines, then the elapsed time.
// Compare a firmware with the LCD console (default) against one built with
// -DGOGO_CONSOLE=0 (see platform/esp32s3/README.md).
//
//	gogo build platform/esp32s3/examples/printbench -o printbench.gogo
package main

import "gogo/actor"

func main() {
	t0 := actor.Now()
	for i := 0; i < 1000; i++ {
		println("line", i, "中文 mixed text 0123456789")
	}
	t1 := actor.Now()
	println("printbench: 1000 lines in", t1-t0, "ms")
}
