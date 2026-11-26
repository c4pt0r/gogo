// recorddemo: records the microphone to /flash/rec with gogo/sound and reads
// the files back. On unix the sound comes from the stdin script (`mic tone`
// lines of 250 ms, 300 ms apart; run with --fs-root). The directory already
// holds 0007.wav and notes.txt, so the numbering continues at 0008. With
// maxSec 1 the recording rotates after 1000 ms into a second file, which a
// Stop ends at 500 ms (its header is patched in place with the true size).
// Right after that report the program plays that file: the recorder has
// given the microphone back by then. Nothing depends on real-time pacing
// but the Stop: it comes about 300 ms after the last tone line (more than
// the 100 ms margin the 50 ms mic poll needs). Everything is read in pieces
// of 1000 bytes, so it also runs under the 4096-word limit of a board.
package main

import (
	"gogo/actor"
	"gogo/audio"
	"gogo/fs"
	"gogo/sound"
	"gogo/wav"
)

func main() {
	fs.Mkdir("/flash/rec")
	fs.WriteFile("/flash/rec/0007.wav", "x")
	fs.WriteFile("/flash/rec/notes.txt", "x")
	r, ok := sound.Record("/flash/rec", audio.Mic{Rate: 8000, FrameMs: 100}, 1)
	if !ok {
		println("no mic")
		return
	}
	n := 0
	for n < 1 { // wait for the first file: it is full after 1 s of sound
		switch m := actor.Receive().(type) {
		case sound.Recorded:
			show(m)
			n++
		}
	}
	actor.Sleep(900) // the first file closes at about 900 ms into the script; the last tone line is read at 1500 ms: Stop at about 1800 ms
	actor.Send(r, sound.Stop{})
	last := ""
	switch m := actor.Receive().(type) {
	case sound.Recorded:
		show(m)
		last = m.Path
	}
	_, ok = sound.Play(last) // the mic is already closed
	println("play", ok)
	switch m := actor.Receive().(type) {
	case sound.Played:
		println("played", m.Bytes, "dones", m.Dones, "err", int(m.Err))
	}
	names, _ := fs.ReadDir("/flash/rec")
	println("files", len(names))
}

func show(m sound.Recorded) {
	println("recorded", m.Path, "ms", m.Ms, "dropped", m.Dropped, "err", int(m.Err))
	i, _ := fs.Stat(m.Path)
	f, _ := fs.Open(m.Path, fs.Read)
	h, _ := f.Read(wav.HeaderLen)
	hd, ok := wav.Info(h)
	peak := 0
	for {
		b, _ := f.Read(1000)
		if b.Len() == 0 {
			break
		}
		for k := 0; k+1 < b.Len(); k += 2 {
			v := int(b.At(k)) | int(b.At(k+1))<<8
			if v >= 32768 {
				v = 65536 - v
			}
			if v > peak {
				peak = v
			}
		}
	}
	f.Close()
	println("  size", i.Size, "header ok", ok, "rate", hd.Rate, "data", hd.Data, "samples", (i.Size-wav.HeaderLen)/2, "peak", peak)
}
