// The simulated mic and speaker on unix: `mic tone` and `mic silence`
// script lines become 16-bit PCM frames (an integer sine at amplitude 1000
// doubled every 6 dB of Gain: 4000 at 12 dB), one Chunk per FrameMs; the
// ports are half duplex; the speaker takes at most 4096 bytes per write
// (`spk: <n> bytes` on stderr) and answers each Out with Done once it has
// played. 350 ms of sound at FrameMs 100 is 3 whole frames (the half frame
// left is discarded on close). The script's last sleep gives the polls
// more than 100 ms of margin.
package main

import (
	"gogo/actor"
	"gogo/audio"
	"gogo/bytes"
	"gogo/port"
)

func sample(d bytes.Bytes, i int) int {
	v := int(d.At(2*i)) | int(d.At(2*i+1))<<8
	if v >= 32768 {
		v -= 65536
	}
	return v
}

func main() {
	mic, ok := port.Open("mic", audio.Mic{Rate: 8000, FrameMs: 100, Gain: 13})
	println("mic", ok)
	_, ok = port.Open("spk", audio.Speaker{Rate: 8000, Volume: 50})
	println("spk while mic", ok)
	for n := 0; n < 3; {
		switch c := actor.Receive().(type) {
		case port.Chunk:
			peak, last := 0, 0
			for i := 0; i < c.Data.Len()/2; i++ {
				v := sample(c.Data, i)
				if v > peak {
					peak = v
				}
				if v != 0 {
					last = i
				}
			}
			println("chunk", c.Seq, "dropped", c.Dropped, "bytes", c.Data.Len(), "first", sample(c.Data, 0), sample(c.Data, 1), sample(c.Data, 2), "peak", peak, "last sound", last)
			n++
		}
	}
	port.Close(mic)
	spk, ok := port.Open("spk", audio.Speaker{Rate: 8000, Volume: 50})
	println("spk", ok)
	_, ok = port.Open("mic", audio.Mic{Rate: 8000})
	println("mic while spk", ok)
	b := bytes.NewBuffer(10000)
	for i := 0; i < 10000; i++ {
		b.WriteByte(0)
	}
	pcm := b.Freeze()
	actor.Send(spk, port.Out{Data: pcm})
	actor.Send(spk, port.Out{Data: pcm.Slice(0, 99)})
	for n := 0; n < 2; {
		switch d := actor.Receive().(type) {
		case port.Done:
			println("done", d.Seq)
			n++
		}
	}
}
