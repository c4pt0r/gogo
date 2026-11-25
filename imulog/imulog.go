// Package imulog records the "imu" port to files, in the background: the
// offline logger. It is written in the gogo subset on top of gogo/imu,
// gogo/port and gogo/fs and compiled into every image that imports it.
//
// Start opens the imu port with a Config and gives it to a small actor that
// appends every Chunk and every Gesture to dir as a binary record. Read
// returns the records again, oldest first, whether the logger still runs or
// not (after a reboot, say).
//
// Files: dir/0.imu is the newest, dir/1.imu the one before. Once 0.imu
// exceeds maxBytes/2, 1.imu is deleted and 0.imu renamed to 1.imu, like
// gogo/ringlog, so the total stays within maxBytes plus one record. Every
// record is one Write plus Sync on a freshly opened file, so a power loss
// costs at most the last record; a torn tail is ignored by Read.
//
// A record is a 15-byte header, little-endian, then the payload:
//
//	0      kind     1 chunk, 2 gesture
//	1..4   a        chunk: Seq;     gesture: Kind
//	5..8   timeMs   chunk: TimeMs;  gesture: the millisecond clock when the
//	                actor got it (low 31 bits)
//	9..12  b        chunk: Dropped; gesture: Value
//	13..14 len      payload bytes (the chunk's Data; 0 for a gesture)
//
// One logger per dir: a second Start on the same dir writes into the same
// files (the fs gives each Append a Busy and the record is lost, counted
// nowhere), so do not. A write that fails (fs.NoSpace, a missing card) loses
// that record; the logger goes on.
package imulog

import (
	"gogo/actor"
	"gogo/bytes"
	"gogo/fs"
	"gogo/imu"
	"gogo/port"
)

// Record kinds.
const (
	ChunkRec   = 1
	GestureRec = 2
)

// HeaderLen is the size of a record's header.
const HeaderLen = 15

// Record is one logged event. For a ChunkRec, A is the Chunk's Seq, B its
// Dropped and Data its payload (12-byte samples, see gogo/imu). For a
// GestureRec, A is the gesture Kind, B its Value, and Data is empty.
type Record struct {
	Kind, TimeMs, A, B int
	Data               bytes.Bytes
}

// Start opens the imu port with cfg and starts the logging actor, which
// owns the port from then on; killing the returned PID (actor.Kill) stops
// the log and closes the port. ok is false if the port does not open (no
// IMU, a bad cfg, the port taken, no memory) or maxBytes is under 64; dir
// is created (one level) at the first record.
func Start(cfg imu.Config, dir string, maxBytes int) (actor.PID, bool) {
	if maxBytes < 64 {
		return actor.Self(), false
	}
	p, ok := port.Open("imu", cfg)
	if !ok {
		return actor.Self(), false
	}
	l := actor.Spawn(func() {
		for {
			switch m := actor.Receive().(type) {
			case port.Chunk:
				Append(dir, maxBytes, ChunkRec, m.Seq, m.TimeMs, m.Dropped, m.Data)
			case imu.Gesture:
				Append(dir, maxBytes, GestureRec, m.Kind, int(actor.Now()&0x7fffffff), m.Value, bytes.FromString(""))
			}
		}
	})
	if !port.Handoff(p, l) { // events before this go to the caller; there are none yet
		port.Close(p)
		actor.Kill(l, actor.Killed)
		return l, false
	}
	return l, true
}

func put32(b bytes.Buffer, v int) {
	b.WriteByte(byte(v))
	b.WriteByte(byte(v >> 8))
	b.WriteByte(byte(v >> 16))
	b.WriteByte(byte(v >> 24))
}

// Append writes one record to the log in dir; Start's actor calls it, and a
// program may add its own markers (kind 3 and up are free). A payload over
// 65535 bytes is fs.NoMemory.
func Append(dir string, maxBytes int, kind, a, timeMs, b int, data bytes.Bytes) fs.Err {
	n := data.Len()
	if n > 65535 {
		return fs.NoMemory
	}
	buf := bytes.NewBuffer(HeaderLen + n)
	buf.WriteByte(byte(kind))
	put32(buf, a)
	put32(buf, timeMs)
	put32(buf, b)
	buf.WriteByte(byte(n))
	buf.WriteByte(byte(n >> 8))
	buf.WriteBytes(data)
	p := dir + "/0.imu"
	f, e := fs.Open(p, fs.Append)
	if e == fs.NotFound {
		fs.Mkdir(dir)
		f, e = fs.Open(p, fs.Append)
	}
	if e != fs.OK {
		return e
	}
	e = f.Write(buf.Freeze())
	if e == fs.OK {
		e = f.Sync()
	}
	sz, e2 := f.Size()
	f.Close()
	if e != fs.OK {
		return e
	}
	if e2 != fs.OK {
		return e2
	}
	if sz > maxBytes/2 {
		if e := fs.Remove(dir + "/1.imu"); e != fs.OK && e != fs.NotFound {
			return e
		}
		return fs.Rename(p, dir+"/1.imu")
	}
	return fs.OK
}

func get32(b bytes.Bytes, i int) int {
	return int(b.At(i)) | int(b.At(i+1))<<8 | int(b.At(i+2))<<16 | int(b.At(i+3))<<24
}

// parse appends the whole records of file p to out and returns it; a torn
// or missing tail, or a missing file, ends it quietly.
func parse(p string, out []Record) []Record {
	i, e := fs.Stat(p)
	if e != fs.OK || i.Size == 0 {
		return out
	}
	b, e := fs.ReadFile(p, i.Size)
	if e != fs.OK {
		return out
	}
	n := b.Len()
	off := 0
	for off+HeaderLen <= n {
		l := int(b.At(off+13)) | int(b.At(off+14))<<8
		if off+HeaderLen+l > n {
			break
		}
		out = append(out, Record{
			Kind:   int(b.At(off)),
			A:      get32(b, off+1),
			TimeMs: get32(b, off+5),
			B:      get32(b, off+9),
			Data:   b.Slice(off+HeaderLen, off+HeaderLen+l),
		})
		off += HeaderLen + l
	}
	return out
}

// Read returns the newest max records of the log in dir, oldest first;
// fewer if there are fewer, none if there is no log or max is 0 or less.
// It reads both files into memory (up to maxBytes), charged to the caller;
// fs.NoMemory is not reported, the log just reads as empty then, so call
// it from an actor with room.
func Read(dir string, max int) []Record {
	var all []Record
	if max <= 0 {
		return all
	}
	all = parse(dir+"/1.imu", all)
	all = parse(dir+"/0.imu", all)
	if len(all) <= max {
		return all
	}
	var out []Record
	for i := len(all) - max; i < len(all); i++ {
		out = append(out, all[i])
	}
	return out
}
