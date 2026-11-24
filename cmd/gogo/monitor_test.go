package main

import (
	"bytes"
	"io"
	"strings"
	"testing"
	"time"
)

func TestCaptureStopsAtUntil(t *testing.T) {
	r, w := io.Pipe()
	go func() {
		w.Write([]byte("boot\ngogo dev: image 8 bytes\nhel"))
		w.Write([]byte("lo\ngogo: vm exit"))
		w.Write([]byte("ed (0), arena used 0\nignored"))
		// never closed: capture must stop on its own
	}()
	var out bytes.Buffer
	start := time.Now()
	if err := capture(r, &out, 5*time.Second, "gogo: vm exited"); err != nil {
		t.Fatal(err)
	}
	if time.Since(start) > 2*time.Second {
		t.Error("capture waited for the timeout despite the until string")
	}
	got := out.String()
	if !strings.Contains(got, "hello\ngogo: vm exited (0), arena used 0\n") {
		t.Errorf("output = %q", got)
	}
	if strings.Contains(got, "ignored") {
		t.Errorf("output past the until line: %q", got)
	}
}

func TestCaptureTimeout(t *testing.T) {
	r, w := io.Pipe()
	go w.Write([]byte("partial"))
	var out bytes.Buffer
	start := time.Now()
	if err := capture(r, &out, 200*time.Millisecond, "never"); err != nil {
		t.Fatal(err)
	}
	if d := time.Since(start); d < 150*time.Millisecond || d > 2*time.Second {
		t.Errorf("returned after %v, want ~200ms", d)
	}
	if out.String() != "partial" {
		t.Errorf("output = %q", out.String())
	}
}

func TestCaptureEOF(t *testing.T) {
	var out bytes.Buffer
	if err := capture(strings.NewReader("abc"), &out, 5*time.Second, ""); err != nil {
		t.Fatal(err)
	}
	if out.String() != "abc" {
		t.Errorf("output = %q", out.String())
	}
}

func TestMonitorArgs(t *testing.T) {
	o, err := parseMonitorArgs([]string{"-port", "/dev/x", "-t", "7", "-until", "gogo: vm exited", "-reset"})
	if err != nil {
		t.Fatal(err)
	}
	if o.port != "/dev/x" || o.dur != 7*time.Second || o.until != "gogo: vm exited" || !o.reset {
		t.Errorf("opts = %+v", o)
	}
	o, err = parseMonitorArgs(nil)
	if err != nil || o.dur != 10*time.Second || o.reset || o.port != "" {
		t.Errorf("defaults = %+v, %v", o, err)
	}
	for _, bad := range [][]string{{"-t"}, {"-t", "x"}, {"-t", "0"}, {"extra"}, {"-port"}} {
		if _, err := parseMonitorArgs(bad); err == nil {
			t.Errorf("parseMonitorArgs(%q) accepted", bad)
		}
	}
}
