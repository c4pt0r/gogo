package main

// gogo monitor: non-interactive serial capture from the board.
//
//	gogo monitor [-port P] [-timeout seconds] [-until text] [-reset]
//
// Copies the board's console to stdout for -timeout seconds (default 10), or
// until a line containing -until has arrived. -reset first resets the chip
// (RTS pulse, as esptool's hard reset does on the USB-Serial/JTAG port), so
// the capture starts at boot. Without -reset, opening and closing the port
// never touches the chip: the modem lines (reset/boot strap on this port)
// are only changed by -reset. -port defaults like `gogo flash`. -t is the
// old name of -timeout, kept (unlisted, with a note on stderr) for one
// release.

import (
	"bytes"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"time"
)

const monitorUsage = "usage: gogo monitor [-port P] [-timeout seconds] [-until text] [-reset]"

type monitorOpts struct {
	port  string
	dur   time.Duration
	until string
	reset bool
	oldT  bool // the deprecated -t was used
}

// secondsFlag is a positive number of seconds.
type secondsFlag struct{ d *time.Duration }

func (s secondsFlag) String() string {
	if s.d == nil {
		return ""
	}
	return s.d.String()
}
func (s secondsFlag) Set(v string) error {
	secs, err := strconv.ParseFloat(v, 64)
	if err != nil || !(secs > 0 && secs < 1e9) {
		return fmt.Errorf("want a positive number of seconds")
	}
	*s.d = time.Duration(secs * float64(time.Second))
	return nil
}

func parseMonitorArgs(args []string) (monitorOpts, error) {
	o := monitorOpts{dur: 10 * time.Second}
	fs := newFlags("monitor")
	fs.StringVar(&o.port, "port", "", "")
	fs.Var(secondsFlag{&o.dur}, "timeout", "")
	fs.Var(secondsFlag{&o.dur}, "t", "") // deprecated: -timeout
	fs.StringVar(&o.until, "until", "", "")
	fs.BoolVar(&o.reset, "reset", false, "")
	pos, err := parseFlags(fs, args, monitorUsage)
	if err != nil {
		return o, err
	}
	if len(pos) != 0 {
		return o, usageError("monitor", "no arguments expected", monitorUsage)
	}
	o.oldT = flagSet(fs, "t")
	return o, nil
}

// capture copies r to w until dur has passed, r ends, or (with until != "")
// a complete line containing until has been copied; nothing after that
// line is written.
func capture(r io.Reader, w io.Writer, dur time.Duration, until string) error {
	chunks := make(chan []byte)
	done := make(chan error, 1)
	go func() {
		for {
			buf := make([]byte, 4096)
			n, err := r.Read(buf)
			if n > 0 {
				chunks <- buf[:n]
			}
			if err != nil {
				if err == io.EOF {
					err = nil
				}
				done <- err
				return
			}
		}
	}()
	timer := time.NewTimer(dur)
	defer timer.Stop()
	var line []byte // the incomplete last line, for the until match
	for {
		select {
		case b := <-chunks:
			if until == "" {
				if _, err := w.Write(b); err != nil {
					return err
				}
				continue
			}
			for len(b) > 0 {
				i := bytes.IndexByte(b, '\n')
				part := b
				if i >= 0 {
					part = b[:i+1]
				}
				if _, err := w.Write(part); err != nil {
					return err
				}
				b = b[len(part):]
				line = append(line, part...)
				if i >= 0 {
					if bytes.Contains(line, []byte(until)) {
						return nil
					}
					line = line[:0]
				}
			}
		case err := <-done:
			return err
		case <-timer.C:
			return nil
		}
	}
}

func monitorCmd(args []string) error {
	o, err := parseMonitorArgs(args)
	if err != nil {
		return err
	}
	if o.oldT {
		fmt.Fprintln(os.Stderr, "gogo monitor: -t is deprecated, use -timeout")
	}
	if o.port == "" {
		if o.port, err = detectPort(filepath.Glob); err != nil {
			return err
		}
	}
	f, err := openSerial(o.port)
	if err != nil {
		return err
	}
	defer f.Close()
	if o.reset {
		if err := resetChip(f); err != nil {
			return err
		}
	}
	return capture(f, os.Stdout, o.dur, o.until)
}
