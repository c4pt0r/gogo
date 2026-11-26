package main

import (
	"bytes"
	"errors"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
	"time"
)

func TestParseFlags(t *testing.T) {
	cases := []struct {
		args []string
		o    string
		v    bool
		pos  []string
	}{
		{[]string{"a", "b"}, "", false, []string{"a", "b"}},
		{[]string{"-o", "x", "a"}, "x", false, []string{"a"}},
		{[]string{"--o", "x", "a"}, "x", false, []string{"a"}},
		{[]string{"a", "-o", "x"}, "x", false, []string{"a"}},
		{[]string{"a", "--o=x", "b", "-v"}, "x", true, []string{"a", "b"}},
		{[]string{"-v", "--", "-o", "x"}, "", true, []string{"-o", "x"}},
		{[]string{"a", "-", "b"}, "", false, []string{"a", "-", "b"}},
		{nil, "", false, nil},
	}
	for _, c := range cases {
		fs := newFlags("t")
		o := fs.String("o", "", "")
		v := fs.Bool("v", false, "")
		pos, err := parseFlags(fs, c.args, "usage: t")
		if err != nil {
			t.Errorf("%q: %v", c.args, err)
			continue
		}
		if *o != c.o || *v != c.v || !reflect.DeepEqual(pos, c.pos) {
			t.Errorf("%q: o=%q v=%v pos=%q, want %q %v %q", c.args, *o, *v, pos, c.o, c.v, c.pos)
		}
	}
	fs := newFlags("t")
	fs.String("o", "", "")
	if _, err := parseFlags(fs, []string{"a", "-x"}, "usage: t"); err == nil || !strings.Contains(err.Error(), "gogo t: flag provided but not defined: -x\nusage: t") {
		t.Errorf("unknown flag: %v", err)
	}
	if _, err := parseFlags(fs, []string{"a", "-o"}, "usage: t"); err == nil || !strings.Contains(err.Error(), "needs an argument") {
		t.Errorf("missing value: %v", err)
	}
	var help helpError
	if _, err := parseFlags(fs, []string{"a", "--help"}, "usage: t"); !errors.As(err, &help) || help.usage != "usage: t" {
		t.Errorf("-help: %v", err)
	}
}

// Every command takes its flags before or after its arguments, with one
// or two dashes.
func TestCommandsTakeFlagsAnywhere(t *testing.T) {
	o, err := parseMonitorArgs([]string{"--timeout", "2.5", "-until", "x", "--reset"})
	if err != nil || o.dur != 2500*time.Millisecond || o.until != "x" || !o.reset || o.oldT {
		t.Errorf("monitor: %+v %v", o, err)
	}
	o, err = parseMonitorArgs([]string{"-t", "3"})
	if err != nil || o.dur != 3*time.Second || !o.oldT {
		t.Errorf("monitor -t: %+v %v", o, err)
	}
	for _, bad := range [][]string{{"-timeout", "0"}, {"-timeout", "-1"}, {"-timeout", "inf"}, {"-timeout", "5s"}} {
		if _, err := parseMonitorArgs(bad); err == nil {
			t.Errorf("monitor %q accepted", bad)
		}
	}

	// config: a value starting with "-" is a value, flags may follow it.
	t.Setenv("GOGO_CONFIG", filepath.Join(t.TempDir(), "cfg"))
	var out, errOut bytes.Buffer
	none := func(string) ([]string, error) { return nil, nil }
	if err := configMain([]string{"set", "offset", "-5", "-local"}, &out, nil, &errOut, none); err != nil {
		t.Fatal(err)
	}
	if err := configMain([]string{"get", "--local", "offset"}, &out, nil, &errOut, none); err != nil || out.String() != "-5\n" {
		t.Errorf("config get = %q, %v", out.String(), err)
	}

	// flash: the file before the flags.
	var ran []string
	f := flasher{glob: none, run: func(name string, args []string) error { ran = append([]string{name}, args...); return nil }}
	app := writeImg(t, t.TempDir(), "app.ggb")
	if err := f.flash([]string{app, "--port", "/dev/p", "-esptool=/x/esptool"}); err != nil {
		t.Fatal(err)
	}
	if len(ran) < 3 || ran[0] != "/x/esptool" || ran[2] != "/dev/p" {
		t.Errorf("esptool run = %q", ran)
	}
}

// gogo firmware -port asks the board which one it is.
func TestFirmwareDetectsBoard(t *testing.T) {
	old := detectBoard
	defer func() { detectBoard = old }()
	var asked string
	detectBoard = func(esptool, port string) (string, error) { asked = port; return "sticks3", nil }
	dir := t.TempDir()
	img := writeImg(t, dir, "a.ggb")
	var out bytes.Buffer
	if err := firmwareCmd([]string{img, "-port", "/dev/p", "-o", filepath.Join(dir, "fw")}, &out, nil); err != nil {
		t.Fatal(err)
	}
	if asked != "/dev/p" || !strings.Contains(out.String(), "board: sticks3 (detected on /dev/p)") {
		t.Errorf("asked %q, out %q", asked, out.String())
	}
}
