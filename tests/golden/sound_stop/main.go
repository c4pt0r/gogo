// sound_stop: a Stop while the recorder has no file open still gets exactly
// one final Recorded, with Path "" and Ms 0 (nothing written since the last
// one). Case 1: Stop before the first Chunk (the script's first mic line
// comes 300 ms later). Case 2: maxSec 1 and a 1000 ms tone: the file fills
// with the 10th frame and is reported, then a Stop comes before any other
// Chunk (the script sleeps). No file is made in case 1, one in case 2.
package main

import (
	"gogo/actor"
	"gogo/audio"
	"gogo/fs"
	"gogo/sound"
)

func main() {
	r, ok := sound.Record("/flash/a", audio.Mic{Rate: 8000, FrameMs: 100}, 0)
	println("record a", ok)
	actor.Send(r, sound.Stop{})
	wait()

	r, ok = sound.Record("/flash/b", audio.Mic{Rate: 8000, FrameMs: 100}, 1)
	println("record b", ok)
	wait() // the full file
	actor.Send(r, sound.Stop{})
	wait() // the final report: no file open
	for _, d := range []string{"/flash/a", "/flash/b"} {
		names, _ := fs.ReadDir(d)
		println(d, "files", len(names))
	}
}

func wait() {
	for {
		switch m := actor.Receive().(type) {
		case sound.Recorded:
			println("recorded path", "\""+m.Path+"\"", "ms", m.Ms, "dropped", m.Dropped, "err", int(m.Err))
			return
		}
	}
}
