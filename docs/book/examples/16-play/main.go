// playdemo: makes a 10000-byte WAV (written in 10 pieces) (5000 samples at 8 kHz, 625 ms) with
// gogo/wav and plays it with gogo/sound. Play sends 4096-byte Outs, two in
// flight, so 10000 bytes are 3 Outs (4096, 4096, 1808) and 3 Dones; the
// unix speaker prints `spk: <n> bytes` on stderr for each write. The player
// plays in real time, but nothing here depends on it: the program waits
// for Played.
package main

import (
	"gogo/actor"
	"gogo/bytes"
	"gogo/fs"
	"gogo/sound"
	"gogo/wav"
)

func main() {
	w, e := wav.Create("/flash/ramp.wav", 8000, 10000)
	println("create", int(e))
	for part := 0; part < 10; part++ { // pieces of 1000 bytes: fits a board's actor limit
		b := bytes.NewBuffer(1000)
		for i := part * 500; i < part*500+500; i++ {
			v := (i % 100) * 300
			b.WriteByte(byte(v))
			b.WriteByte(byte(v >> 8))
		}
		w, e = wav.Write(w, b.Freeze())
	}
	println("write", int(e), w.N)
	println("close", int(wav.Close(w)))

	_, ok := sound.Play("/flash/none.wav")
	println("missing file", ok)
	fs.WriteFile("/flash/bad.wav", "this is not a wav file, it is far too short to be one")
	_, ok = sound.Play("/flash/bad.wav")
	println("bad file", ok)

	_, ok = sound.Play("/flash/ramp.wav")
	println("play", ok)
	switch m := actor.Receive().(type) {
	case sound.Played:
		println("played", m.Path, "bytes", m.Bytes, "dones", m.Dones, "err", int(m.Err))
	}
}
