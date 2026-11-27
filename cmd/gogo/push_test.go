package main

import (
	"bufio"
	"bytes"
	"encoding/base64"
	"fmt"
	"hash/crc32"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"testing"
)

// simBoard plays the board's side of the console frames that push and
// device use (config_esp.c), over a pipe, and records what it was sent.
type simBoard struct {
	mu       sync.Mutex
	files    map[string][]byte
	frames   []string
	formats  int
	failAt   int    // a data frame at this offset is answered with an error (0: never)
	corrupt  bool   // store a byte other than the one sent
	oldFirm  bool   // a firmware without push and format-flash
	params   string // the VM parameters its info line carries ("": none, an older firmware)
	pending  []byte
	pushTo   string
	pushLen  int
	pushCRC  uint32
	toHost   *io.PipeWriter
	fromHost *io.PipeReader
}

// conn returns the host's end of the console.
func (b *simBoard) conn() io.ReadWriteCloser {
	hr, bw := io.Pipe() // board -> host
	br, hw := io.Pipe() // host -> board
	b.toHost, b.fromHost = bw, br
	if b.files == nil {
		b.files = map[string][]byte{}
	}
	go b.serve()
	return struct {
		io.Reader
		io.Writer
		io.Closer
	}{hr, hw, hw}
}

func (b *simBoard) say(s string) { io.WriteString(b.toHost, "program output\x02"+s+"\n") }

func (b *simBoard) serve() {
	sc := bufio.NewScanner(b.fromHost)
	for sc.Scan() {
		line := strings.TrimPrefix(sc.Text(), "\x02gogo-config ")
		b.mu.Lock()
		b.frames = append(b.frames, line)
		b.mu.Unlock()
		f := strings.Fields(line)
		if b.oldFirm && f[0] != "stats" {
			b.say("err bad key")
			continue
		}
		switch f[0] {
		case "stats":
			b.say("kv heap_free=1")
			b.say("ok")
		case "info":
			p := ""
			if b.params != "" {
				p = b.params + " "
			}
			b.say("ok fw=v0.2.0 board=cardputer image=15.6 " + p + "provides=gpio,kbd,/flash,/sd")
		case "format-flash":
			b.formats++
			b.say("ok restarting to format /flash")
		case "push":
			b.pushTo, b.pending = f[1], nil
			b.pushLen, _ = strconv.Atoi(f[2])
			c, _ := strconv.ParseUint(f[3], 16, 32)
			b.pushCRC = uint32(c)
			b.say("ok 0")
		case "data":
			off, _ := strconv.Atoi(f[1])
			d, err := base64.StdEncoding.DecodeString(f[2])
			if err != nil || off != len(b.pending) || len(d) > pushChunk {
				b.say("err bad data frame")
				continue
			}
			if b.failAt != 0 && off == b.failAt {
				b.say("err no space on /flash")
				continue
			}
			if b.corrupt {
				d[0] ^= 1
			}
			b.pending = append(b.pending, d...)
			b.say("ok " + strconv.Itoa(len(b.pending)))
		case "end":
			if len(b.pending) != b.pushLen || crc32.ChecksumIEEE(b.pending) != b.pushCRC {
				b.say("err the stored file does not match (length or CRC)")
				continue
			}
			b.files[b.pushTo] = b.pending
			b.say("ok")
		case "abort":
			b.say("ok")
		}
	}
}

func pushRun(t *testing.T, b *simBoard, args ...string) (string, error) {
	t.Helper()
	var out bytes.Buffer
	err := pushMain(args, pushEnv{out: &out, glob: globOf("/dev/cu.usbmodem1"),
		open: func(string) (io.ReadWriteCloser, error) { return b.conn(), nil }})
	return out.String(), err
}

func TestPush(t *testing.T) {
	dir := t.TempDir()
	app := writeImg(t, dir, "app.ggb")
	img := readFile(t, app)
	// A module of several chunks, to the default name and to a path.
	big := writeImg(t, dir, "big.ggb", strings.Split(strings.Repeat("drv,", 200), ",")[:200]...)
	b := &simBoard{}
	if out, err := pushRun(t, b, app); err != nil || !strings.Contains(out, "pushed "+app+" to /flash/app.ggb") {
		t.Fatalf("push: %q %v", out, err)
	}
	if out, err := pushRun(t, b, big, "/flash/mods/worker.ggb", "-port", "/dev/x"); err != nil {
		t.Fatalf("push big: %q %v", out, err)
	}
	if !bytes.Equal(b.files["/flash/app.ggb"], img) || !bytes.Equal(b.files["/flash/mods/worker.ggb"], readFile(t, big)) {
		t.Errorf("board files differ from what was pushed")
	}
	if n := len(readFile(t, big)); n <= 2*pushChunk {
		t.Fatalf("big image is only %d bytes", n)
	}

	// The board refusing a chunk stops the push and aborts it.
	b = &simBoard{failAt: pushChunk}
	if _, err := pushRun(t, b, big); err == nil || !strings.Contains(err.Error(), "no space on /flash") {
		t.Errorf("full board: %v", err)
	}
	if last := b.frames[len(b.frames)-1]; last != "abort" {
		t.Errorf("last frame %q, want abort", last)
	}
	if _, ok := b.files["/flash/big.ggb"]; ok {
		t.Error("a failed push left the target")
	}
	// What the board stored is not what was sent: the board refuses the end.
	b = &simBoard{corrupt: true}
	if _, err := pushRun(t, b, app); err == nil || !strings.Contains(err.Error(), "does not match") {
		t.Errorf("corrupt: %v", err)
	}
	// An older firmware.
	b = &simBoard{oldFirm: true}
	if _, err := pushRun(t, b, app); err == nil || !strings.Contains(err.Error(), "no push (update it)") {
		t.Errorf("old firmware: %v", err)
	}

	// Refused before the board is touched.
	junk := writeFile(t, dir, "junk.ggb", "not an image")
	for _, c := range []struct {
		args []string
		why  string
	}{
		{[]string{junk}, "not a gogo image"},
		{[]string{app, "/sd/app.ggb"}, "not a path under /flash/"},
		{[]string{app, "/flash/../x.ggb"}, "not a path under /flash/"},
		{[]string{app, "/flash/a b.ggb"}, "bad name"},
		{[]string{app, "/flash/x."}, "bad name"},
		{[]string{app, "/flash/mods/.gogo-push"}, "pick another"},
		{[]string{app, "/flash/" + strings.Repeat("n", 33)}, "bad name"},
		{[]string{}, "usage: gogo push"},
		{[]string{app, "/flash/a", "/flash/b"}, "usage: gogo push"},
	} {
		b := &simBoard{}
		if _, err := pushRun(t, b, c.args...); err == nil || !strings.Contains(err.Error(), c.why) {
			t.Errorf("push %q: %v, want %q", c.args, err, c.why)
		}
		if len(b.frames) != 0 {
			t.Errorf("push %q sent %q", c.args, b.frames)
		}
	}
}

func deviceRun(t *testing.T, b *simBoard, stdin string, args ...string) (string, string, error) {
	t.Helper()
	var out, errOut bytes.Buffer
	err := deviceMain(args, deviceEnv{out: &out, errOut: &errOut, in: strings.NewReader(stdin), glob: globOf("/dev/cu.usbmodem1"),
		open: func(string) (io.ReadWriteCloser, error) { return b.conn(), nil }})
	return out.String(), errOut.String(), err
}

func TestDevice(t *testing.T) {
	b := &simBoard{}
	if out, _, err := deviceRun(t, b, "", "stats"); err != nil || out != "heap_free=1\n" {
		t.Errorf("stats: %q %v", out, err)
	}
	// format-flash asks first; anything but "yes" sends nothing.
	for _, answer := range []string{"", "y\n", "no\n", "YES please\n"} {
		b := &simBoard{}
		if _, prompt, err := deviceRun(t, b, answer, "format-flash"); err == nil || !strings.Contains(prompt, "deletes every file on /flash") || len(b.frames) != 0 {
			t.Errorf("answer %q: err %v, frames %q, prompt %q", answer, err, b.frames, prompt)
		}
	}
	b = &simBoard{}
	if out, _, err := deviceRun(t, b, "yes\n", "format-flash"); err != nil || b.formats != 1 || !strings.Contains(out, "restarts and formats /flash") {
		t.Errorf("format-flash: %q %v (%d formats)", out, err, b.formats)
	}
	b = &simBoard{}
	if _, prompt, err := deviceRun(t, b, "", "format-flash", "-yes"); err != nil || b.formats != 1 || prompt != "" {
		t.Errorf("format-flash -yes: %v %q", err, prompt)
	}
	if fmt.Sprint(b.frames) != "[format-flash]" {
		t.Errorf("frames %q: only the format command may be sent", b.frames)
	}
	b = &simBoard{oldFirm: true}
	if _, _, err := deviceRun(t, b, "", "-yes", "format-flash"); err == nil || !strings.Contains(err.Error(), "no format-flash (update it)") {
		t.Errorf("old firmware: %v", err)
	}
	want := "firmware  gogo v0.2.0\nboard     cardputer (Cardputer ADV)\nimage     format 15.6 (runs images 15.0 to 15.6)\nprovides  gpio kbd /flash /sd\n"
	if out, _, err := deviceRun(t, &simBoard{}, "", "info"); err != nil || out != want {
		t.Errorf("info: %q %v", out, err)
	}
	// A firmware that reports its VM parameters: one line of them.
	ps := "actor_memory=16K memory=160K max_actors=1024 max_ports=16 max_links=8 code_mem=64K bytes_handles=64/16"
	want = "firmware  gogo v0.2.0\nboard     cardputer (Cardputer ADV)\nimage     format 15.6 (runs images 15.0 to 15.6)\n" +
		"limits    " + ps + "\nprovides  gpio kbd /flash /sd\n"
	if out, _, err := deviceRun(t, &simBoard{params: ps}, "", "info"); err != nil || out != want {
		t.Errorf("info with parameters: %q %v", out, err)
	}
	if _, _, err := deviceRun(t, &simBoard{oldFirm: true}, "", "info"); err == nil || !strings.Contains(err.Error(), "this firmware has no info") {
		t.Errorf("info on an old firmware: %v", err)
	}
	if _, _, err := deviceRun(t, &simBoard{}, "", "erase"); err == nil || !strings.Contains(err.Error(), "unknown operation") {
		t.Errorf("erase: %v", err)
	}
}

func readFile(t *testing.T, p string) []byte {
	t.Helper()
	b, err := os.ReadFile(p)
	if err != nil {
		t.Fatal(err)
	}
	return b
}

func writeFile(t *testing.T, dir, name, content string) string {
	t.Helper()
	p := filepath.Join(dir, name)
	if err := os.WriteFile(p, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
	return p
}
