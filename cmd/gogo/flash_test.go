package main

import (
	"encoding/binary"
	"errors"
	"hash/crc32"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"

	"gogotools/compiler"
)

// fakeImage returns n bytes that pass the image magic check.
func fakeImage(n int) []byte {
	img := make([]byte, n)
	binary.LittleEndian.PutUint32(img, imageMagic)
	for i := 4; i < n; i++ {
		img[i] = byte(i * 7)
	}
	if n >= 8 {
		binary.LittleEndian.PutUint32(img[4:], compiler.ImageVersion)
	}
	return img
}

func TestPackImageHeader(t *testing.T) {
	img := fakeImage(64)
	out := packImage(img)
	if len(out) != 16+len(img) {
		t.Fatalf("len = %d, want %d", len(out), 16+len(img))
	}
	le := binary.LittleEndian
	if got := le.Uint32(out[0:]); got != 0x4D494747 {
		t.Errorf("magic = %#x, want 0x4D494747", got)
	}
	if string(out[0:4]) != "GGIM" {
		t.Errorf("magic bytes = %q, want GGIM", out[0:4])
	}
	if got := le.Uint32(out[4:]); got != uint32(len(img)) {
		t.Errorf("image_bytes = %d, want %d", got, len(img))
	}
	if got, want := le.Uint32(out[8:]), crc32.ChecksumIEEE(img); got != want {
		t.Errorf("crc = %#x, want %#x", got, want)
	}
	if got := le.Uint32(out[12:]); got != 0 {
		t.Errorf("reserved = %d, want 0", got)
	}
	if !reflect.DeepEqual(out[16:], img) {
		t.Error("image bytes not copied unchanged")
	}
}

func TestPackImageRejects(t *testing.T) {
	notImage := fakeImage(64)
	notImage[0] ^= 0xFF
	cases := map[string][]byte{
		"empty":     nil,
		"short":     {0x47, 0x4F},
		"bad magic": notImage,
		"unaligned": fakeImage(66),
		"too big":   fakeImage(partSize - 16 + 4),
	}
	old := fakeImage(64)
	binary.LittleEndian.PutUint32(old[4:], compiler.ImageVersion-1)
	cases["old version"] = old
	cases["truncated header"] = fakeImage(4)
	for name, img := range cases {
		if out := packImage(img); out != nil {
			t.Errorf("%s: packImage accepted it", name)
		}
		if err := checkImage(img); err == nil {
			t.Errorf("%s: checkImage accepted it", name)
		}
	}
	// The largest image that fits still packs.
	if out := packImage(fakeImage(partSize - 16)); len(out) != partSize {
		t.Errorf("max-size image: len = %d, want %d", len(out), partSize)
	}
}

func globOf(matches ...string) func(string) ([]string, error) {
	return func(pattern string) ([]string, error) {
		if pattern != "/dev/cu.usbmodem*" {
			return nil, errors.New("unexpected pattern " + pattern)
		}
		return matches, nil
	}
}

func TestDetectPort(t *testing.T) {
	p, err := detectPort(globOf("/dev/cu.usbmodem4101"))
	if err != nil || p != "/dev/cu.usbmodem4101" {
		t.Errorf("one port: got %q, %v", p, err)
	}
	if _, err := detectPort(globOf()); err == nil || !strings.Contains(err.Error(), "-port") {
		t.Errorf("no port: err = %v, want a hint to use -port", err)
	}
	_, err = detectPort(globOf("/dev/cu.usbmodem1", "/dev/cu.usbmodem2"))
	if err == nil || !strings.Contains(err.Error(), "/dev/cu.usbmodem2") {
		t.Errorf("two ports: err = %v, want both listed", err)
	}
}

func TestFlashRunsEsptool(t *testing.T) {
	dir := t.TempDir()
	app := filepath.Join(dir, "app.ggb")
	img := fakeImage(32)
	if err := os.WriteFile(app, img, 0o644); err != nil {
		t.Fatal(err)
	}
	var gotArgs []string
	var gotData []byte
	f := flasher{
		glob: globOf("/dev/cu.usbmodem9"),
		run: func(name string, args []string) error {
			gotArgs = append([]string{name}, args...)
			data, err := os.ReadFile(args[len(args)-1])
			gotData = data
			return err
		},
	}
	if err := f.flash([]string{app, "-esptool", "/x/esptool"}); err != nil {
		t.Fatal(err)
	}
	want := []string{"/x/esptool", "--port", "/dev/cu.usbmodem9", "write-flash", "0x210000"}
	if len(gotArgs) != len(want)+1 || !reflect.DeepEqual(gotArgs[:len(want)], want) {
		t.Fatalf("args = %q, want %q + <tmp>", gotArgs, want)
	}
	if !reflect.DeepEqual(gotData, packImage(img)) {
		t.Error("temp file is not header+image")
	}
	if _, err := os.Stat(gotArgs[len(gotArgs)-1]); !os.IsNotExist(err) {
		t.Error("temp file not removed")
	}

	// -port overrides detection; flags may come first.
	f.glob = globOf()
	if err := f.flash([]string{"-port", "/dev/ttyX", "-esptool", "/x/esptool", app}); err != nil {
		t.Fatal(err)
	}
	if gotArgs[2] != "/dev/ttyX" {
		t.Errorf("port = %q, want /dev/ttyX", gotArgs[2])
	}

	// An esptool failure is reported.
	f.run = func(string, []string) error { return errors.New("boom") }
	if err := f.flash([]string{"-port", "/dev/ttyX", "-esptool", "/x/esptool", app}); err == nil {
		t.Error("esptool failure not reported")
	}
}

func TestFlashRejects(t *testing.T) {
	dir := t.TempDir()
	bad := filepath.Join(dir, "bad.ggb")
	if err := os.WriteFile(bad, []byte("not an image"), 0o644); err != nil {
		t.Fatal(err)
	}
	good := filepath.Join(dir, "good.ggb")
	if err := os.WriteFile(good, fakeImage(16), 0o644); err != nil {
		t.Fatal(err)
	}
	ran := false
	f := flasher{
		glob: globOf(), // no board: auto-detection fails
		run:  func(string, []string) error { ran = true; return nil },
	}
	for _, args := range [][]string{
		{"-port", "/dev/x", "-esptool", "e", bad},
		{},
		{"a.ggb", "b.ggb"},
		{"-port"},
		{"-bogus", good},
		{"-port", "/dev/x", "-esptool", "e", filepath.Join(dir, "missing.ggb")},
		{"-esptool", "e", good}, // no -port and no board
	} {
		if err := f.flash(args); err == nil {
			t.Errorf("flash(%q) succeeded", args)
		}
	}
	if ran {
		t.Error("esptool ran for a rejected request")
	}
}
