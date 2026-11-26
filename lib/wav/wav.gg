// Package wav writes and parses WAV files (RIFF/WAVE, PCM, 16-bit,
// mono, the 44-byte canonical header). It is written in the gogo subset on
// top of gogo/fs and compiled into every image that imports it.
//
// Create writes the header at once, with the size the caller expects
// (planned bytes of PCM; 0 if unknown). Close then makes it true: when
// the PCM written differs from the plan, it patches the 44 header bytes in
// place with File.WriteAt, which never extends the file, so it needs no new
// space (on /sd) and no copy: it works on a full card, and a recording
// stopped early costs 44 bytes of I/O, however long it is. A file whose
// writing failed half way (a full medium) gets a header that is true for
// the whole frames before the failure; the partial write after it is
// trailing bytes the header does not count. If the patch itself fails
// (/flash is copy-on-write and needs a free block), Close returns the error
// and the header still says the planned size; players that trust the file
// size (gogo/sound does) play it fine.
//
// Writer is a value: every call returns the new state, as the subset has
// no methods or pointers.
package wav

import (
	"gogo/bytes"
	"gogo/fs"
)

// HeaderLen is the size of the header Create writes.
const HeaderLen = 44

// Writer is a file being written. Treat the fields as read-only: N is the
// PCM bytes written so far.
type Writer struct {
	F       fs.File
	Path    string
	Rate    int
	Planned int
	N       int
}

// Header is what Info finds in a WAV header: Rate in Hz, Data the size the
// "data" chunk declares (negative if it does not fit an int, as the
// 0xFFFFFFFF of a live stream).
type Header struct {
	Rate int
	Data int
}

func put16(b bytes.Buffer, v int) {
	b.WriteByte(byte(v))
	b.WriteByte(byte(v >> 8))
}

func put32(b bytes.Buffer, v int) {
	b.WriteByte(byte(v))
	b.WriteByte(byte(v >> 8))
	b.WriteByte(byte(v >> 16))
	b.WriteByte(byte(v >> 24))
}

// header builds the 44 bytes for dataLen bytes of PCM at rate.
func header(rate, dataLen int) bytes.Bytes {
	b := bytes.NewBuffer(HeaderLen)
	b.WriteString("RIFF")
	put32(b, 36+dataLen)
	b.WriteString("WAVEfmt ")
	put32(b, 16)
	put16(b, 1) // PCM
	put16(b, 1) // mono
	put32(b, rate)
	put32(b, rate*2) // bytes per second
	put16(b, 2)      // block align
	put16(b, 16)     // bits per sample
	b.WriteString("data")
	put32(b, dataLen)
	return b.Freeze()
}

// Create makes (or truncates) path and writes the header for planned bytes
// of PCM (0 if unknown: Close then always rewrites a non-empty file).
func Create(path string, rate, planned int) (Writer, fs.Err) {
	var w Writer
	f, e := fs.Open(path, fs.Write)
	if e != fs.OK {
		return w, e
	}
	if e = f.Write(header(rate, planned)); e != fs.OK {
		f.Close()
		return w, e
	}
	return Writer{F: f, Path: path, Rate: rate, Planned: planned}, fs.OK
}

// Write appends PCM (an even number of bytes). On an error the writer
// still counts only what was written before; call Close to finish the file.
func Write(w Writer, pcm bytes.Bytes) (Writer, fs.Err) {
	e := w.F.Write(pcm)
	if e != fs.OK {
		return w, e
	}
	return Writer{F: w.F, Path: w.Path, Rate: w.Rate, Planned: w.Planned, N: w.N + pcm.Len()}, fs.OK
}

// Close commits and closes the file and makes its header true (see above).
// It reports a failed patch or commit; the file is closed either way.
func Close(w Writer) fs.Err {
	var e fs.Err
	if w.N != w.Planned {
		e = w.F.WriteAt(0, header(w.Rate, w.N))
	}
	if e2 := w.F.Sync(); e == fs.OK {
		e = e2
	}
	w.F.Close()
	return e
}

func get32(b bytes.Bytes, i int) int {
	return int(b.At(i)) | int(b.At(i+1))<<8 | int(b.At(i+2))<<16 | int(b.At(i+3))<<24
}

// Info parses the first 44 bytes of a WAV file: ok is false unless it is
// the canonical RIFF/WAVE, PCM, mono, 16-bit header this package writes.
func Info(h bytes.Bytes) (Header, bool) {
	var z Header
	if h.Len() < HeaderLen {
		return z, false
	}
	if !h.Slice(0, 4).Equal(bytes.FromString("RIFF")) || !h.Slice(8, 16).Equal(bytes.FromString("WAVEfmt ")) || !h.Slice(36, 40).Equal(bytes.FromString("data")) {
		return z, false
	}
	if get32(h, 16) != 16 || int(h.At(20))|int(h.At(21))<<8 != 1 || int(h.At(22))|int(h.At(23))<<8 != 1 || int(h.At(34))|int(h.At(35))<<8 != 16 {
		return z, false
	}
	return Header{Rate: get32(h, 24), Data: get32(h, 40)}, true
}
