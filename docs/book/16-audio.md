# 16. Audio

Both boards have a microphone and a speaker, on one codec and one I2S bus.
Two ports give programs this hardware, `"mic"` and `"spk"` (types in
package `gogo/audio`), and two small libraries built on them record to
WAV files and play WAV files back: `gogo/wav` and `gogo/sound`.

The ports are **streams** (chapter 15): the microphone sends `port.Chunk`
messages, the speaker takes `port.Out` commands and answers each with
`port.Done`. The sound is 16-bit little-endian mono PCM at 8000 or 16000
samples per second.

## 16.1 The ports

```go
type Mic struct{ Rate, FrameMs, Gain int }
type Speaker struct{ Rate, Volume int }
```

```go
mic, ok := port.Open("mic", audio.Mic{Rate: 16000, FrameMs: 100})
spk, ok := port.Open("spk", audio.Speaker{Rate: 16000, Volume: 60})
```

- `Mic.Rate` is 8000 or 16000. `FrameMs` is how much sound one `Chunk`
  holds (0 means 100; 20 to 500). `Gain` is extra gain, 0 to 24 dB in
  steps of 6, above the board's normal microphone level: 0 is normal
  (speech at arm's length is clearly above the noise floor and does not
  clip). Each platform maps it to its own codec gain.
  Frames are whole: a half frame left when you close the port is dropped.
- `Speaker.Volume` is 0 (mute) to 100: 1 to 100 runs from 40 dB below
  the board's loudest level up to it. Send `port.Out{Data: pcm}` (an even,
  non-empty number of bytes). Each `Out` is answered with `Done` once it
  has played. **Keep two `Out`s in flight**: send two, then one more for each
  `Done`. The driver writes the second right after the first, so the
  speaker never runs dry between them; with one `Out` at a time you hear a
  click at every seam.
- The ports are **half duplex**: while one is open the other refuses to
  open (`ok == false`). You cannot record and play at the same time.
- A mic that cannot keep up does not stop the program: a `Chunk` that does
  not fit the owner's memory quota, or sound the hardware lost, is counted
  in the next `Chunk`'s `Dropped` (whole frames).

### Memory for a recorder

The mic port charges its owner for its frame buffer (`F` words, where
`F = Rate * FrameMs / 2000`; a word is 4 bytes) and for every `Chunk`
waiting in the mailbox (`F + 10` words). An actor that stalls for 300 ms
on an SD write finds 3 more frames waiting. At 16 kHz and 100 ms frames
that is about 4000 words, which a board's default per-actor limit (4096
words, chapter 6) does not leave room for. A program that writes
sound to the SD card therefore spawns its recorder with
`actor.Options{MaxMemory}`, and lets the recorder open the mic itself:
the open checks the charge (and room for one `Chunk`) against the
opener's limit, so a long frame opened in a small actor and then handed
over fails. `gogo/sound` does this for you.

## 16.2 Recording: `gogo/sound`

```go
func Record(dir string, cfg audio.Mic, maxSec int) (actor.PID, bool)

type Stop struct{}
type Recorded struct {
	Path    string
	Ms      int
	Dropped int
	Err     fs.Err
}
```

`Record` starts a recorder actor, which opens the mic with `cfg` and owns
it; `Record` waits for that open, so `ok` still says whether the mic opened.
It writes every `Chunk` to `dir/NNNN.wav`, where `NNNN` is four digits: the
numbering **continues from the highest `NNNN.wav` already in `dir`**, so a
reboot does not overwrite anything. `dir` is created (one level) if it does
not exist. After `maxSec` seconds of sound (0 means 600) the file is
closed and the next `Chunk` starts the next file, so a long recording is
a series of files of at most ten minutes. A file always holds a whole
number of frames.

Every finished file is reported to the actor that called `Record` as one
`sound.Recorded`: `Ms` is the sound in the file, `Dropped` the frames the mic
lost while it was written (their time is missing from the file). Send
`sound.Stop{}` to the recorder to finish the current file and end it.
A `Stop` always gets exactly one final `Recorded`: if it comes while no
file is open (right after a file filled up, or before the first `Chunk`),
that `Recorded` has `Path == ""` and `Ms == 0`, meaning nothing was written
since the last one. `ok`
is false if the mic does not open (no microphone, the speaker is open,
a bad `cfg`) or `dir` cannot be used.

```go
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
```

The script is `mic tone <hz> <ms>` lines (section 16.5). The output:

```text
recorded /flash/rec/0008.wav ms 1000 dropped 0 err 0
  size 16044 header ok true rate 8000 data 16000 samples 8000 peak 999
recorded /flash/rec/0009.wav ms 500 dropped 0 err 0
  size 8044 header ok true rate 8000 data 8000 samples 4000 peak 999
play true
played 8000 dones 2 err 0
files 4
```

The directory already held `0007.wav` and `notes.txt`, so the numbering
continues at `0008`. The first file holds 1000 ms (8000 samples, 16000
bytes of PCM plus the 44-byte header); the second was stopped after 500 ms
and its header patched, then played back. The programs read files in
pieces of 1000 bytes: they also run under the 4096-word actor limit of a
board.

### Errors: a full card

A failed write, a full card or a removed card **stops the recorder**. It
finishes the file (the header covers the whole frames written before the
failure), gives the microphone back, sends a `Recorded` whose `Err` is the
`fs.Err` (`fs.NoSpace`, `fs.IO`, ...) and exits normally. It never
deletes anything to make room: that is the program's decision (it can
remove old files and call `Record` again, and the numbering carries on).
Monitor the recorder (`actor.Monitor`) if you want to see it end. After a
`Stop` or an error the microphone is already closed when `Recorded`
arrives, so the program may call `sound.Play` at once (the example does);
the player likewise closes the speaker before it sends `Played`. The
recording length is capped at one hour per file (`sound.MaxMaxSec`).

### The WAV header and `wav.Close`

A WAV header holds the size of the data, which a recorder only knows at
the end. `gogo/wav` writes the header first, with the size you **expect**
(a recorder's file is a fixed number of frames), and `wav.Close` makes it
true: when the file ends as planned it only commits, and when it ends early
(a `Stop`, a failed write) it patches the 44 header bytes **in place** with
`File.WriteAt` (chapter 9). That is 44 bytes of I/O however long the
recording is, needs no copy, and does not need free space on an SD card, so
the header is right even when the card is full. On `/flash`
(copy-on-write) the patch needs one free block; if it fails, `Close`
returns the error and the header keeps the planned size. Players and
`sound.Play` trust the file size, so such a file still plays.

## 16.3 Playing: `gogo/sound`

```go
func Play(path string) (actor.PID, bool)

type Played struct {
	Path  string
	Bytes int
	Dones int
	Err   fs.Err
}
```

`Play` opens the WAV file, opens the speaker at the file's rate and starts
a player actor, which reads the file in 4096-byte pieces and keeps two
`Out`s in flight (section 16.1). When it is over (or after `sound.Stop{}`)
the caller gets one `sound.Played`: `Bytes` of PCM handed to the speaker,
`Dones` the `Out`s that completed, `Err` is `fs.OK`, a read error or
`sound.SpkFailed`. `ok` is false, and nothing starts, if the file is
missing, is not a canonical 16-bit mono PCM WAV at 8000 or 16000 Hz (the
files `Record` writes are), or the speaker does not open (the mic is open,
no speaker). The size of the file is the truth, not the header: the file
`Record` could not finish plays to its end.

```go
// playdemo: makes a 10000-byte WAV (5000 samples at 8 kHz, 625 ms) with
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
	b := bytes.NewBuffer(10000)
	for i := 0; i < 5000; i++ {
		v := (i % 100) * 300
		b.WriteByte(byte(v))
		b.WriteByte(byte(v >> 8))
	}
	w, e = wav.Write(w, b.Freeze())
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
```

```text
create 0
write 0 10000
close 0
missing file false
bad file false
play true
played /flash/ramp.wav bytes 10000 dones 3 err 0
```

and on stderr, one line from the simulated speaker for each write:

```text
spk: 4096 bytes
spk: 4096 bytes
spk: 1808 bytes
```

10000 bytes are three `Out`s (4096, 4096, 1808) and three `Done`s.

## 16.4 `gogo/wav`

`wav` is the file format on its own, for programs that make sound
themselves (a synthesiser, a recorder with other rules). `Close` patches the header
through `File.WriteAt`:

```go
func Create(path string, rate, planned int) (Writer, fs.Err)
func Write(w Writer, pcm bytes.Bytes) (Writer, fs.Err)
func Close(w Writer) fs.Err
func Info(header bytes.Bytes) (Header, bool)
```

The subset has no methods or pointers, so a `Writer` is a value and each
call returns the new one: `w, e = wav.Write(w, pcm)`. `planned` is the
number of PCM bytes you expect (0 if you do not know: `Close` then always
patches the header). `Info` parses the 44-byte header (`ok` is false for anything
but canonical RIFF/WAVE, PCM, mono, 16-bit); `Header.Data` is the size
the header declares, which may be more than the file holds.

## 16.5 On the host

On the host VM the microphone and speaker are simulated, like the keyboard:

| line | effect |
|---|---|
| `mic tone <hz> <ms>` | a sine wave (amplitude 1000, doubled every 6 dB of `Gain`) |
| `mic silence <ms>` | zeros |
| `sleep <ms>` | wait before the next line |

Sound arrives as a burst at the next poll (every `min(FrameMs, 50)` ms),
not in real time, so a long `mic tone` line is delivered at once and can
fill a recorder's mailbox: write tones of a few hundred milliseconds with a
`sleep` after each, as the example does, and keep a margin of at least
100 ms between script lines and anything that depends on them.
The speaker prints `spk: <n> bytes` on stderr for every write (at most 4096
bytes) and plays in real time, so `Done` arrives as late as on the board.

`--fs-root` gives `/flash` a directory on the host; for testing a full
medium `gogovm` has the flag `--fs-quota <bytes>` (not in `gogo run`).
Each medium (`/flash`, and `/sd` if present) then holds at most that many
bytes of files: what is already in its directory counts from the start,
deleting or truncating a file gives its bytes back, a write that does not
fit stores what does and fails with `fs.NoSpace`, and `fs.Usage` reports
the bytes used and the quota. `--fs-quota 0` is a full medium. The golden
test `tests/golden/sound_diskfull` uses it.

## 16.6 Requirements and firmware

The compiler records in the image that a program opens `"mic"` or `"spk"`
itself (a requirement, like the other ports); a firmware built without them
refuses that image at boot. Importing `gogo/sound` does not require them: the library
declares `//gogo:optional mic spk` (chapter 12), and `Record` and `Play`
return false when the hardware is absent. A program that opens `"mic"` or
`"spk"` with `port.Open` itself still requires them. Both are on by default and share one switch, `GOGO_AUDIO` (`gogo
firmware` names them `mic` and `spk`). A board without the hardware
refuses `port.Open` (`ok == false`). The codec's I2S pins are reserved: the
`gpio` port refuses G14 to G18 on the StickS3 and G41, G42, G43 and G46 on
the Cardputer ADV.

On both boards the microphone and speaker are an ES8311 codec on I2S:

- `Gain` 0 is 54 dB of codec gain (30 dB analog, 24 dB in the ADC);
  `Gain` adds digital gain on top.
- `Volume` 1 to 100 is the codec's DAC volume from 40 dB below the ceiling
  to the ceiling, in 0.4 dB steps. The ceiling is 0 dB on the Cardputer
  ADV and -4 dB on the StickS3: at full codec level its USB console once
  stopped answering (probably a supply dip from the amplifier).
- The codec's analog reference is switched on by the first `mic` or `spk`
  open and **stays on until the board resets**. Powering it down at every
  close made each later open start with about a second of silence. The
  first `mic` open after power-up still has that silence: a first
  recording after a cold boot starts with about 1 s of zeros.
