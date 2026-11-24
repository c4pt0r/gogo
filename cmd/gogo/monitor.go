package main

// gogo monitor: non-interactive serial capture from the board.
//
//	gogo monitor [-port P] [-t seconds] [-until text] [-reset]
//
// Copies the board's console to stdout for -t seconds (default 10), or
// until a line containing -until has arrived. -reset first resets the chip
// (RTS pulse, as esptool's hard reset does on the USB-Serial/JTAG port), so
// the capture starts at boot. Without -reset, opening and closing the port
// never touches the chip: the modem lines (reset/boot strap on this port)
// are only changed by -reset. -port defaults like `gogo flash`.

import (
	"bytes"
	"errors"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"time"
)

const monitorUsage = "usage: gogo monitor [-port P] [-t seconds] [-until text] [-reset]"

type monitorOpts struct {
	port  string
	dur   time.Duration
	until string
	reset bool
}

func parseMonitorArgs(args []string) (monitorOpts, error) {
	o := monitorOpts{dur: 10 * time.Second}
	for i := 0; i < len(args); i++ {
		switch a := args[i]; a {
		case "-reset":
			o.reset = true
		case "-port", "-t", "-until":
			if i+1 >= len(args) {
				return o, errors.New(monitorUsage)
			}
			v := args[i+1]
			i++
			switch a {
			case "-port":
				o.port = v
			case "-until":
				o.until = v
			default:
				secs, err := strconv.ParseFloat(v, 64)
				if err != nil || secs <= 0 {
					return o, errors.New(monitorUsage)
				}
				o.dur = time.Duration(secs * float64(time.Second))
			}
		default:
			return o, errors.New(monitorUsage)
		}
	}
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
