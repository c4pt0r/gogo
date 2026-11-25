// Package audio is the gogo audio API: the "mic" and "spk" ports. It is a
// stub package like gogo/imu: the compiler type-checks against it and the
// bodies (there are none) are never run. The C side is the common audio
// driver (runtime/input/audio.c) on top of the board's codec. Recording to WAV files
// and playing them back are in the library gogo/sound, which imports this
// package (a stub package cannot also hold library code).
//
// Open the microphone with port.Open("mic", audio.Mic{Rate: 16000, FrameMs:
// 100}); each frame arrives as one port.Chunk of 16-bit little-endian mono
// PCM. Open the speaker with port.Open("spk", audio.Speaker{Rate: 16000,
// Volume: 60}), send port.Out PCM and each is answered with port.Done once
// it has played; keep two Outs in flight (send the next on each Done) to
// play without gaps. The two ports are half duplex: while one is open,
// opening the other returns ok=false.
//
// The struct types Mic and Speaker have fixed type indices 42 and 43 in
// every image (TYPE_AUDIO_MIC, TYPE_AUDIO_SPEAKER in runtime/vm/opcodes.h); keep
// their field order in sync with compiler.reservedTypes.
package audio

// Mic configures the microphone: Rate is 8000 or 16000 samples per second,
// FrameMs (0 means 100; 20 to 500) is how much sound one Chunk holds, Gain
// is extra gain in dB (0 to 24, in steps of 6) above the platform's normal
// mic level: 0 is normal (speech at arm's length clearly above the noise
// floor, without clipping), and each platform maps it to its codec's gain.
//
// Memory: with F = Rate*FrameMs/2000 words (one frame), the port charges
// its owner F words, and each Chunk waiting in the mailbox takes F+10 more.
// Open fails (ok=false) when not even one Chunk fits beside the owner's
// heap. To ride out a stall of S ms (an SD card write) without Dropped,
// the owner needs heap + F + (ceil(S/FrameMs) + 1) * (F+10) words (the
// +1 is the Chunk it is handling): at 16 kHz, 100 ms and a 300 ms stall
// that is heap + 4040 words (a word is 4 bytes), more than the 4096-word
// default on a board, so a recorder is spawned with
// actor.Options{MaxMemory}. Dropped counts whole frames lost.
type Mic struct{ Rate, FrameMs, Gain int }

// Speaker configures the speaker: Rate is 8000 or 16000 samples per second,
// Volume is 0 (mute) to 100: 1 to 100 runs from 40 dB below the board's
// loudest level up to it (the StickS3 caps that at -4 dB of the codec's
// range; see the platform README).
type Speaker struct{ Rate, Volume int }

// The sample rates both ports accept.
const (
	Rate8k  = 8000
	Rate16k = 16000
)

// Limits and defaults of the Mic and Speaker fields.
const (
	DefaultFrameMs = 100
	MinFrameMs     = 20
	MaxFrameMs     = 500
	MaxGain        = 24
	MaxVolume      = 100
)
