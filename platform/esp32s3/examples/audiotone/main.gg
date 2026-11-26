// Speaker half of the two-board audio check: after 1 s, plays a 1 kHz tone
// (16 kHz, volume 60) for 6 s, then 2 s of silence, then exits. Run it on
// one board while audiolisten runs on the other, placed side by side: the
// listener's level should rise for those 6 s.
//
//	gogo build platform/esp32s3/examples/audiotone -o tone.gogo && gogo flash tone.gogo
package main

import (
	"gogo/actor"
	"gogo/audio"
	"gogo/bytes"
	"gogo/port"
)

func main() {
	actor.Sleep(1000)
	p, ok := port.Open("spk", audio.Speaker{Rate: 16000, Volume: 60})
	if !ok {
		println("tone: no spk")
		return
	}
	const per = 2000 // samples per Out: 125 ms
	outs := 6 * 16000 / per
	t0 := actor.Now()
	println("tone: start")
	sent := 0
	done := 0
	for sent < 2 {
		actor.Send(p, port.Out{Data: wave(per)})
		sent++
	}
	for done < outs {
		switch actor.Receive().(type) {
		case port.Done:
			done++
			if sent < outs {
				actor.Send(p, port.Out{Data: wave(per)})
				sent++
			}
		}
	}
	println("tone: end, ms", int(actor.Now()-t0))
	port.Close(p)
	actor.Sleep(2000)
}

// wave is samples of a 1 kHz sine at 16 kHz, amplitude 16000.
func wave(samples int) bytes.Bytes {
	sine := []int{0, 6123, 11314, 14782, 16000, 14782, 11314, 6123, 0, -6123, -11314, -14782, -16000, -14782, -11314, -6123}
	b := bytes.NewBuffer(samples * 2)
	for i := 0; i < samples; i++ {
		v := sine[i%16]
		b.WriteByte(byte(v))
		b.WriteByte(byte(v >> 8))
	}
	return b.Freeze()
}
