// Package ringlog is a size-bounded log in two files, dir/0.log (newest)
// and dir/1.log, written in the gogo subset on top of gogo/fs and compiled
// into every image that imports it.
//
// Append adds to 0.log; once that exceeds maxBytes/2, 1.log is deleted and
// 0.log renamed to 1.log, so the total stays within maxBytes plus one line.
// Each Append opens, appends, syncs and closes, so a power loss costs at
// most the last line, and an fs.OK result means the line was committed
// (a failed commit is returned, not lost at Close). Append writes line as given: include the newline yourself.
//
// One writer per dir: while any file holds dir/0.log open for writing,
// Append returns fs.Busy and the line is NOT written. Rotation needs
// dir/1.log not to be open (a concurrent Read holds it briefly): if the
// rotation fails, Append returns that error (fs.Busy, say) although the
// line WAS written; the next Append retries the rotation, so the size
// bound holds again once the reader is done and is best-effort until then.
package ringlog

import (
	"gogo/bytes"
	"gogo/fs"
)

// Append adds line (a string, a bytes.Bytes or a []byte) to the log in dir,
// creating dir (one level) if needed.
func Append(dir string, maxBytes int, line any) fs.Err {
	p := dir + "/0.log"
	f, e := fs.Open(p, fs.Append)
	if e == fs.NotFound {
		fs.Mkdir(dir)
		f, e = fs.Open(p, fs.Append)
	}
	if e != fs.OK {
		return e
	}
	e = f.Write(line)
	if e == fs.OK {
		e = f.Sync() // Close cannot report a failed commit; Sync can
	}
	n, e2 := f.Size()
	f.Close()
	if e != fs.OK {
		return e
	}
	if e2 != fs.OK {
		return e2
	}
	if n > maxBytes/2 {
		if e := fs.Remove(dir + "/1.log"); e != fs.OK && e != fs.NotFound {
			return e
		}
		return fs.Rename(p, dir+"/1.log")
	}
	return fs.OK
}

func size(p string) int {
	i, e := fs.Stat(p)
	if e != fs.OK {
		return 0
	}
	return i.Size
}

// Read returns the newest max bytes of the log in dir (1.log then 0.log, in
// order); fewer if there is less, empty if there is none.
func Read(dir string, max int) (bytes.Bytes, fs.Err) {
	s1 := size(dir + "/1.log")
	s0 := size(dir + "/0.log")
	want := s1 + s0
	if max < want {
		want = max
	}
	if want <= 0 {
		return bytes.FromString(""), fs.OK
	}
	skip := s1 + s0 - want
	buf := bytes.NewBuffer(want)
	for i := 1; i >= 0; i-- {
		name := "/0.log"
		sz := s0
		if i == 1 {
			name = "/1.log"
			sz = s1
		}
		if skip >= sz {
			skip -= sz
			continue
		}
		f, e := fs.Open(dir+name, fs.Read)
		if e != fs.OK {
			return bytes.FromString(""), e
		}
		pos := 0
		for {
			c, e := f.Read(4096)
			n := c.Len()
			if e != fs.OK || n == 0 {
				f.Close()
				if e != fs.OK {
					return bytes.FromString(""), e
				}
				break
			}
			if pos+n > skip {
				lo := skip - pos
				if lo < 0 {
					lo = 0
				}
				buf.WriteBytes(c.Slice(lo, n))
			}
			pos += n
		}
		skip = 0
	}
	return buf.Freeze(), fs.OK
}
