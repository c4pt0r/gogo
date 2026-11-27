package main

import (
	"bytes"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"
)

// configRun runs `gogo config args...` against the store file path.
func configRun(t *testing.T, path string, args ...string) (string, error) {
	t.Helper()
	t.Setenv("GOGO_CONFIG", path)
	var out bytes.Buffer
	err := configCmd(args, &out)
	return out.String(), err
}

func TestConfigCommand(t *testing.T) {
	path := filepath.Join(t.TempDir(), "sub", "config")
	steps := []struct {
		args    []string
		out     string
		wantErr string
	}{
		{[]string{"list"}, "", ""},
		{[]string{"get", "net.ip"}, "", "net.ip is not set"},
		{[]string{"set", "net.ip", "10.0.0.9"}, "", ""},
		{[]string{"set", "greeting", "hello  world "}, "", ""},
		{[]string{"set", "wifi.password", "s3cret"}, "", ""},
		{[]string{"set", "empty", ""}, "", ""},
		{[]string{"get", "net.ip"}, "10.0.0.9\n", ""},
		{[]string{"get", "greeting"}, "hello  world \n", ""},
		{[]string{"get", "wifi.password"}, "(set)\n", ""},
		{[]string{"set", "net.ip", "10.0.0.10"}, "", ""},
		{[]string{"list"}, "empty=\ngreeting=hello  world \nnet.ip=10.0.0.10\nwifi.password=(set)\n", ""},
		{[]string{"rm", "greeting"}, "", ""},
		{[]string{"rm", "greeting"}, "", "greeting is not set"},
		{[]string{"list"}, "empty=\nnet.ip=10.0.0.10\nwifi.password=(set)\n", ""},
		{[]string{"set", "bad key", "x"}, "", "bad key"},
		{[]string{"set", "", "x"}, "", "bad key"},
		{[]string{"set", "a=b", "x"}, "", "bad key"},
		{[]string{"set", "\x00args", "x"}, "", "bad key"}, // the key sys.Args reads
		{[]string{"get", "\x00args"}, "", "bad key"},
		{[]string{"rm", "\x00args"}, "", "bad key"},
		{[]string{"set", "much.too.long.key", "x"}, "", "bad key"},
		{[]string{"set", "k", "two\nlines"}, "", "bad value"},
		{[]string{"set", "k", strings.Repeat("x", 1001)}, "", "bad value"},
		{[]string{"set", "k"}, "", "usage"},
		{[]string{"frob"}, "", "usage"},
		{nil, "", "usage"},
	}
	for _, s := range steps {
		out, err := configRun(t, path, s.args...)
		if s.wantErr == "" && err != nil || s.wantErr != "" && (err == nil || !strings.Contains(err.Error(), s.wantErr)) {
			t.Fatalf("%q: err %v, want %q", s.args, err, s.wantErr)
		}
		if out != s.out {
			t.Fatalf("%q: out %q, want %q", s.args, out, s.out)
		}
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if string(data) != "net.ip=10.0.0.10\nwifi.password=s3cret\nempty=\n" {
		t.Fatalf("file %q", data)
	}
	if fi, _ := os.Stat(path); fi.Mode().Perm() != 0o600 {
		t.Fatalf("mode %v, want 0600 (it holds the Wi-Fi password)", fi.Mode())
	}
}

// Lines the command does not understand are kept as they are.
func TestConfigKeepsOtherLines(t *testing.T) {
	path := filepath.Join(t.TempDir(), "config")
	if err := os.WriteFile(path, []byte("# mine\nblink.ms=1\n\nblink.ms=dup\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	if out, err := configRun(t, path, "get", "blink.ms"); err != nil || out != "1\n" {
		t.Fatalf("get: %q %v (the first line wins, as in the VM)", out, err)
	}
	if _, err := configRun(t, path, "set", "blink.ms", "2"); err != nil {
		t.Fatal(err)
	}
	data, _ := os.ReadFile(path)
	if string(data) != "# mine\nblink.ms=2\n\n" {
		t.Fatalf("file %q (set replaces every line of the key)", data)
	}
}

// fakeBoard answers boardConfig's frame with a scripted console stream.
type fakeBoard struct {
	sent  bytes.Buffer
	reply *strings.Reader
}

func (b *fakeBoard) Write(p []byte) (int, error) { return b.sent.Write(p) }
func (b *fakeBoard) Read(p []byte) (int, error) {
	if b.reply.Len() == 0 {
		select {} // the console stays open
	}
	if len(p) > 3 {
		p = p[:3] // replies arrive in pieces
	}
	return b.reply.Read(p)
}

func TestBoardConfig(t *testing.T) {
	cases := []struct {
		op      string
		args    []string
		console string
		frame   string
		out     string
		wantErr string
	}{
		{"set", []string{"wifi.ssid", "my net"}, "tick 1\nti\x02ok\nck 2\n", "\x02gogo-config set wifi.ssid my net\n", "", ""},
		{"get", []string{"demo.url"}, "noise\x02ok http://x/ y\n", "\x02gogo-config get demo.url\n", "http://x/ y\n", ""},
		{"get", []string{"nope"}, "\x02err not set\n", "\x02gogo-config get nope\n", "", "nope is not set"},
		{"list", nil, "\x02kv wifi.ssid=n\r\n\x02kv demo.url=u\n\x02ok\n", "\x02gogo-config list\n", "demo.url=u\nwifi.ssid=n\n", ""},
		{"rm", []string{"k"}, "\x02err ESP_FAIL\n", "\x02gogo-config rm k\n", "", "board: ESP_FAIL"},
		{"scan", nil, "\x02kv net1 ch 6\n\x02ok\n", "\x02gogo-config scan\n", "net1 ch 6\n", ""},
	}
	for _, c := range cases {
		b := &fakeBoard{reply: strings.NewReader(c.console)}
		var out bytes.Buffer
		err := boardConfig(b, c.op, c.args, &out, time.Second)
		if c.wantErr == "" && err != nil || c.wantErr != "" && (err == nil || !strings.Contains(err.Error(), c.wantErr)) {
			t.Fatalf("%s %q: err %v, want %q", c.op, c.args, err, c.wantErr)
		}
		if b.sent.String() != c.frame || out.String() != c.out {
			t.Fatalf("%s %q: sent %q out %q, want %q %q", c.op, c.args, b.sent.String(), out.String(), c.frame, c.out)
		}
	}
	b := &fakeBoard{reply: strings.NewReader("just program output\n")}
	if err := boardConfig(b, "list", nil, io.Discard, 50*time.Millisecond); err == nil || !strings.Contains(err.Error(), "no answer") {
		t.Fatalf("silent board: %v", err)
	}
}

// Store selection: $GOGO_CONFIG or -local means the file even with a board
// connected; "-" reads the value from stdin.
func TestConfigStoreChoice(t *testing.T) {
	path := filepath.Join(t.TempDir(), "config")
	board := func(string) ([]string, error) { return []string{"/dev/cu.usbmodem-none"}, nil }
	t.Setenv("GOGO_CONFIG", path)
	var out, errOut bytes.Buffer
	if err := configMain([]string{"set", "k", "-"}, &out, strings.NewReader("from stdin\nrest\n"), &errOut, board); err != nil {
		t.Fatal(err)
	}
	t.Setenv("GOGO_CONFIG", "")
	t.Setenv("HOME", t.TempDir())
	if err := configMain([]string{"-local", "list"}, &out, nil, &errOut, board); err != nil {
		t.Fatal(err)
	}
	if err := configMain([]string{"-local", "scan"}, &out, nil, &errOut, board); err == nil || !strings.Contains(err.Error(), "needs a board") {
		t.Fatalf("local scan: %v", err)
	}
	// No GOGO_CONFIG, one board: it is used.
	c := newConfigBench(t, 1)
	c.boards[c.ports[0]].store["k"] = "on the board"
	if o, e, err := c.run("", "get", "k"); err != nil || o != "on the board\n" || !strings.Contains(e, "board on /dev/cu.usbmodemA") {
		t.Fatalf("auto board: %v %q %q", err, o, e)
	}
	if err := configMain([]string{"-port", "p", "-local", "list"}, &out, nil, &errOut, board); err == nil || !strings.Contains(err.Error(), "usage") {
		t.Fatalf("both: %v", err)
	}
	data, _ := os.ReadFile(path)
	if string(data) != "k=from stdin\n" {
		t.Fatalf("file %q", data)
	}
}

// set checks keys and values against the key table.
func TestConfigSchema(t *testing.T) {
	ok := [][2]string{
		{"blink.ms", "250"}, {"my_app.x9", ""}, {"net.ip", "192.168.1.20"}, {"net.mask", "255.255.255.0"},
		{"fs.flash.size", "64K"}, {"fs.flash.size", "1m"}, {"fs.flash.size", "4294967295"}, {"code.mem", "128K"},
		{"code.mem", "2g"}, {"vm.actor_memory", "24K"}, {"vm.actor_memory", "1G"}, {"vm.memory", "2G"}, {"vm.memory", "0"},
		{"vm.max_actors", "65520"}, {"vm.max_ports", "2048"}, {"node.max_links", "1"}, {"vm.handles", "4096"},
		{"vm.handles", "100/10"},
		{"fs.flash.format", "yes"}, {"wifi.ssid", strings.Repeat("s", 32)}, {"wifi.password", "pass word"},
		{"retries", "lots"}, // not a gogo key: any value
	}
	for _, kv := range ok {
		if err := checkSetting(kv[0], kv[1]); err != nil {
			t.Errorf("set %s %q: %v", kv[0], kv[1], err)
		}
	}
	bad := []struct{ k, v, why string }{
		{"Blink", "1", "bad key"},    // upper case: get and rm still take it
		{"blink-ms", "1", "bad key"}, // "-" likewise
		{"sixteen.chars.xx", "", "bad key"},
		{"net.ipp", "1.2.3.4", "belong to gogo"},
		{"gogo.anything", "x", "belong to gogo"},
		{"fs.other", "x", "belong to gogo"},
		{"net.ip", "1.2.3", "dotted IPv4"},
		{"net.ip", "1.2.3.256", "dotted IPv4"},
		{"net.gw", "1.2.3.+4", "dotted IPv4"},
		{"net.dns", "1.2.3.4 ", "dotted IPv4"},
		{"fs.flash.size", "64KB", "K or M"},
		{"fs.flash.size", "4G", "K or M"},
		{"code.mem", "4194304K", "0 to 4294967295"},
		{"code.mem", "", "K, M or G"},
		{"vm.actor_memory", "512", "1K to 1G"},
		{"vm.actor_memory", "2G", "1K to 1G"},
		{"vm.actor_memory", "1T", "K, M or G"},
		{"vm.max_actors", "0", "1 to 65520"},
		{"vm.max_actors", "70000", "1 to 65520"},
		{"vm.max_ports", "4096", "1 to 2048"},
		{"node.max_links", "x", "a decimal integer"},
		{"vm.handles", "70000", "N or N/M"},
		{"vm.handles", "10/20", "N or N/M"},
		{"vm.handles", "10/", "N or N/M"},
		{"vm.other", "1", "belong to gogo"},
		{"fs.flash.format", "true", "want yes"},
		{"wifi.ssid", strings.Repeat("s", 33), "at most 32 bytes"},
		{"wifi.password", strings.Repeat("p", 65), "at most 64 bytes"},
		{"k", "a\x02b", "bad value"},
	}
	for _, c := range bad {
		if err := checkSetting(c.k, c.v); err == nil || !strings.Contains(err.Error(), c.why) {
			t.Errorf("set %s %q: %v, want %q", c.k, c.v, err, c.why)
		}
	}
	// A key set before the table (upper case) can still be read and removed.
	path := filepath.Join(t.TempDir(), "config")
	if err := os.WriteFile(path, []byte("Old-Key=1\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	if out, err := configRun(t, path, "get", "Old-Key"); err != nil || out != "1\n" {
		t.Errorf("get Old-Key: %q %v", out, err)
	}
	if _, err := configRun(t, path, "rm", "Old-Key"); err != nil {
		t.Errorf("rm Old-Key: %v", err)
	}
	var keys bytes.Buffer
	if err := configMain([]string{"keys"}, &keys, nil, &keys, nil); err != nil || !strings.Contains(keys.String(), "code.mem         size    platform") {
		t.Errorf("keys: %v\n%s", err, keys.String())
	}
	// A refused key is refused before a value is read from stdin.
	var out, errOut bytes.Buffer
	if err := configMain([]string{"-local", "set", "net.bogus", "-"}, &out, strings.NewReader("never read\n"), &errOut, nil); err == nil || !strings.Contains(err.Error(), "belong to gogo") {
		t.Errorf("set net.bogus -: %v", err)
	}
}

// The forms gogo device replaces still work, with a note.
func TestConfigDeprecatedForms(t *testing.T) {
	t.Setenv("GOGO_CONFIG", filepath.Join(t.TempDir(), "cfg"))
	var out, errOut bytes.Buffer
	if err := configMain([]string{"-local", "set", "fs.flash.format", "yes"}, &out, nil, &errOut, nil); err != nil || !strings.Contains(errOut.String(), "use gogo device format-flash") {
		t.Errorf("fs.flash.format: %v %q", err, errOut.String())
	}
	errOut.Reset()
	if err := configMain([]string{"-local", "stats"}, &out, nil, &errOut, nil); err == nil || !strings.Contains(errOut.String(), "stats is deprecated, use gogo device stats") {
		t.Errorf("stats: %v %q", err, errOut.String())
	}
}

// node.token is a secret like wifi.password: get and list show "(set)".
func TestConfigNodeTokenHidden(t *testing.T) {
	if got := shown("node.token", "hunter2"); got != "(set)" {
		t.Errorf("shown(node.token) = %q", got)
	}
	if got := shown("node.name", "kitchen"); got != "kitchen" {
		t.Errorf("shown(node.name) = %q", got)
	}
	if _, ok := findCfgKey("node.token"); !ok {
		t.Error("node.token is not in the key table")
	}
	// The firmware sends node.token back; get and list hide it.
	c := newConfigBench(t, 1)
	c.boards[c.ports[0]].store["node.token"] = "hunter2"
	c.boards[c.ports[0]].store["node.name"] = "kitchen"
	if out, _, err := c.run("", "get", "node.token"); err != nil || out != "(set)\n" {
		t.Errorf("board get node.token: %q %v", out, err)
	}
	if out, _, err := c.run("", "list"); err != nil || out != "node.name=kitchen\nnode.token=(set)\n" {
		t.Errorf("board list: %q %v", out, err)
	}
}

// storeBoard is a board's console that answers config frames from a map,
// like platform/esp32s3/main/config_esp.c; failOn makes a set of that key
// fail.
type storeBoard struct {
	mu     sync.Mutex
	store  map[string]string
	sets   []string // keys set, in order
	failOn string
	frames []string
	pw     *io.PipeWriter
	in     []byte
}

func newStoreBoard() *storeBoard {
	return &storeBoard{store: map[string]string{}}
}

// storeConn is one opening of a storeBoard's port.
type storeConn struct {
	*storeBoard
	pr *io.PipeReader
}

// open opens the port anew; the store stays.
func (b *storeBoard) open() io.ReadWriteCloser {
	b.mu.Lock()
	defer b.mu.Unlock()
	pr, pw := io.Pipe()
	b.pw, b.in = pw, nil
	return storeConn{b, pr}
}

func (c storeConn) Read(p []byte) (int, error) { return c.pr.Read(p) }
func (c storeConn) Close() error               { c.pr.Close(); return nil }
func (b *storeBoard) Write(p []byte) (int, error) {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.in = append(b.in, p...)
	for {
		i := bytes.IndexByte(b.in, '\n')
		if i < 0 {
			return len(p), nil
		}
		frame := strings.TrimPrefix(string(b.in[:i]), "\x02gogo-config ")
		b.in = b.in[i+1:]
		b.frames = append(b.frames, frame)
		go io.WriteString(b.pw, "output\n"+b.answer(frame))
	}
}

func (b *storeBoard) answer(frame string) string {
	op, rest, _ := strings.Cut(frame, " ")
	key, value, _ := strings.Cut(rest, " ")
	switch op {
	case "set":
		if key == b.failOn {
			return "\x02err ESP_ERR_NVS_NOT_ENOUGH_SPACE\n"
		}
		b.store[key] = value
		b.sets = append(b.sets, key)
		return "\x02ok\n"
	case "get":
		v, ok := b.store[key]
		if !ok {
			return "\x02err not set\n"
		}
		if key == "wifi.password" {
			v = "(set)"
		}
		return "\x02ok " + v + "\n"
	case "rm":
		if _, ok := b.store[key]; !ok {
			return "\x02err not set\n"
		}
		delete(b.store, key)
		return "\x02ok\n"
	case "list":
		var s string
		for k, v := range b.store {
			if k == "wifi.password" { // the firmware hides only this one
				v = "(set)"
			}
			s += "\x02kv " + k + "=" + v + "\n"
		}
		return s + "\x02ok\n"
	}
	return "\x02err usage\n"
}

// configBench runs gogo config with n boards connected (fake ports
// /dev/cu.usbmodemA, B, ...; each a storeBoard) and stdin.
type configBench struct {
	t      *testing.T
	boards map[string]*storeBoard
	ports  []string
	opened []string
}

func newConfigBench(t *testing.T, n int) *configBench {
	c := &configBench{t: t, boards: map[string]*storeBoard{}}
	for i := 0; i < n; i++ {
		p := "/dev/cu.usbmodem" + string(rune('A'+i))
		c.ports = append(c.ports, p)
		c.boards[p] = newStoreBoard()
	}
	return c
}

func (c *configBench) run(stdin string, args ...string) (out, errOut string, err error) {
	var o, e bytes.Buffer
	env := configEnv{out: &o, errOut: &e, in: strings.NewReader(stdin),
		glob: func(pat string) ([]string, error) {
			if pat != portGlob {
				c.t.Fatalf("glob %q", pat)
			}
			return c.ports, nil
		},
		open: func(p string) (io.ReadWriteCloser, error) {
			c.opened = append(c.opened, p)
			b, ok := c.boards[p]
			if !ok {
				return nil, errors.New("no such port " + p)
			}
			return b.open(), nil
		}}
	err = configExec(args, env)
	return o.String(), e.String(), err
}

// The store: -port, -local and $GOGO_CONFIG choose it; otherwise exactly
// one board must be connected (none or several is an error, never a
// silent write to this computer's file).
func TestConfigTarget(t *testing.T) {
	ops := [][]string{{"set", "blink.ms", "5"}, {"rm", "blink.ms"}, {"get", "blink.ms"}, {"list"}}
	for _, op := range ops {
		for _, n := range []int{0, 1, 2} {
			for _, mode := range []string{"", "-port", "-local", "GOGO_CONFIG"} {
				name := fmt.Sprintf("%s/%d boards/%s", op[0], n, mode)
				home := t.TempDir()
				t.Setenv("HOME", home)
				envPath := filepath.Join(t.TempDir(), "cfg")
				t.Setenv("GOGO_CONFIG", "")
				local := filepath.Join(home, ".gogo", "config")
				if mode == "GOGO_CONFIG" {
					t.Setenv("GOGO_CONFIG", envPath)
					local = envPath
				}
				os.MkdirAll(filepath.Dir(local), 0o700)
				os.WriteFile(local, []byte("blink.ms=1\n"), 0o600)
				c := newConfigBench(t, n)
				for _, b := range c.boards {
					b.store["blink.ms"] = "1"
				}
				args := append([]string(nil), op...)
				switch mode {
				case "-port":
					args = append(args, "-port", "/dev/cu.usbmodemB")
				case "-local":
					args = append([]string{"-local"}, args...)
				}
				_, errOut, err := c.run("", args...)
				switch {
				case mode == "-port":
					// The port is used as given, connected or not.
					if len(c.opened) != 1 || c.opened[0] != "/dev/cu.usbmodemB" {
						t.Errorf("%s: opened %q", name, c.opened)
					}
					if n == 2 && err != nil {
						t.Errorf("%s: %v", name, err)
					}
				case mode == "-local" || mode == "GOGO_CONFIG":
					if err != nil || len(c.opened) != 0 {
						t.Errorf("%s: %v, opened %q", name, err, c.opened)
					}
					data, _ := os.ReadFile(local)
					want := map[string]string{"set": "blink.ms=5\n", "rm": ""}[op[0]]
					if op[0] == "get" || op[0] == "list" {
						want = "blink.ms=1\n"
					}
					if string(data) != want {
						t.Errorf("%s: local file %q, want %q", name, data, want)
					}
				case n == 0:
					if err == nil || err.Error() != "gogo config: no board found (/dev/cu.usbmodem*); connect one, pass -port, or -local for this computer's store" {
						t.Errorf("%s: %v", name, err)
					}
				case n == 1:
					if err != nil || len(c.opened) != 1 || !strings.Contains(errOut, "board on /dev/cu.usbmodemA") {
						t.Errorf("%s: %v, opened %q, %q", name, err, c.opened, errOut)
					}
				case n == 2:
					if err == nil || err.Error() != "gogo config: several boards found (/dev/cu.usbmodemA, /dev/cu.usbmodemB); pick one with -port, or -local for this computer's store" {
						t.Errorf("%s: %v", name, err)
					}
				}
				if mode == "" && n != 1 {
					if len(c.opened) != 0 {
						t.Errorf("%s: opened %q", name, c.opened)
					}
					if data, _ := os.ReadFile(local); string(data) != "blink.ms=1\n" {
						t.Errorf("%s: the local file changed: %q", name, data)
					}
				}
			}
		}
	}
	// A bad value is reported as such, whatever the boards.
	for _, n := range []int{0, 2} {
		t.Setenv("GOGO_CONFIG", "")
		if _, _, err := newConfigBench(t, n).run("", "set", "fs.flash.size", "64KB"); err == nil || !strings.Contains(err.Error(), "bad value for fs.flash.size") {
			t.Errorf("bad value with %d boards: %v", n, err)
		}
	}
	// keys needs no store: it works with no board, and with several.
	for _, n := range []int{0, 2} {
		t.Setenv("GOGO_CONFIG", "")
		if out, _, err := newConfigBench(t, n).run("", "keys"); err != nil || !strings.Contains(out, "wifi.ssid") {
			t.Errorf("keys with %d boards: %v", n, err)
		}
	}
}

// A secret goes to a board as it is; to the local store (chosen with
// -local or $GOGO_CONFIG) with a warning that the file is plain text. Its
// value never appears in the output. With no board and no choice nothing
// is written and the value is not even read.
func TestConfigSecretTarget(t *testing.T) {
	for _, key := range configSecrets {
		t.Setenv("HOME", t.TempDir())
		t.Setenv("GOGO_CONFIG", "")
		c := newConfigBench(t, 0)
		stdin := strings.NewReader("hunter2\n")
		var o, e bytes.Buffer
		err := configExec([]string{"set", key, "-"}, configEnv{out: &o, errOut: &e, in: stdin, glob: func(string) ([]string, error) { return nil, nil }, open: nil})
		if err == nil || !strings.Contains(err.Error(), "no board found") || stdin.Len() != len("hunter2\n") {
			t.Errorf("%s, no board: %v (stdin left %d)", key, err, stdin.Len())
		}
		for _, mode := range []string{"-local", "GOGO_CONFIG"} {
			path := filepath.Join(os.Getenv("HOME"), ".gogo", "config")
			args := []string{"set", key, "-"}
			if mode == "-local" {
				args = append(args, "-local")
			} else {
				path = filepath.Join(t.TempDir(), "cfg")
				t.Setenv("GOGO_CONFIG", path)
			}
			out, errOut, err := c.run("hunter2\n", args...)
			if err != nil || !strings.Contains(errOut, "warning: "+key+" is kept in plain text in "+path) {
				t.Errorf("%s %s: %v %q", key, mode, err, errOut)
			}
			if strings.Contains(out+errOut, "hunter2") {
				t.Errorf("%s %s: the secret is in the output: %q %q", key, mode, out, errOut)
			}
			if data, _ := os.ReadFile(path); string(data) != key+"=hunter2\n" {
				t.Errorf("%s %s: file %q", key, mode, data)
			}
			t.Setenv("GOGO_CONFIG", "")
		}
		c = newConfigBench(t, 1)
		out, errOut, err := c.run("", "set", key, "hunter2")
		if err != nil || strings.Contains(errOut, "warning") || c.boards[c.ports[0]].store[key] != "hunter2" {
			t.Errorf("%s on a board: %v %q", key, err, errOut)
		}
		if strings.Contains(out+errOut, "hunter2") {
			t.Errorf("%s on a board: the secret is in the output: %q %q", key, out, errOut)
		}
	}
}

const importFile = `# my boards
wifi.ssid=Home Net

net.ip=192.168.1.20
  # an indented comment
demo.url=http://x/?a=b&c=d
greeting=  spaced  ` + "\r" + `
wifi.password=hunter2
`

// import: comments and blank lines skipped, the value to the end of the
// line ("=" in it, blanks kept, a CRLF line end dropped), file order, the
// password never shown.
func TestConfigImport(t *testing.T) {
	want := map[string]string{"wifi.ssid": "Home Net", "net.ip": "192.168.1.20", "demo.url": "http://x/?a=b&c=d", "greeting": "  spaced  ", "wifi.password": "hunter2"}
	order := []string{"wifi.ssid", "net.ip", "demo.url", "greeting", "wifi.password"}
	wantOut := "set wifi.ssid\nset net.ip\nset demo.url\nset greeting\nset wifi.password (set)\n"
	file := filepath.Join(t.TempDir(), "boards.cfg")
	os.WriteFile(file, []byte(importFile), 0o600)
	t.Setenv("GOGO_CONFIG", "")
	t.Setenv("HOME", t.TempDir())

	// To the one board connected, from a file, the flag after it.
	c := newConfigBench(t, 1)
	out, errOut, err := c.run("", "import", file)
	b := c.boards[c.ports[0]]
	if err != nil || out != wantOut+"imported 5 keys to board /dev/cu.usbmodemA\n" {
		t.Fatalf("board import: %v\n%s", err, out)
	}
	if fmt.Sprint(b.sets) != fmt.Sprint(order) || fmt.Sprint(b.store) != fmt.Sprint(want) {
		t.Fatalf("board: sets %q store %q", b.sets, b.store)
	}
	if strings.Contains(out+errOut, "hunter2") {
		t.Fatalf("the password is in the output: %q %q", out, errOut)
	}

	// From stdin, to -port with two boards.
	c = newConfigBench(t, 2)
	if out, _, err := c.run(importFile, "import", "-", "-port", "/dev/cu.usbmodemB"); err != nil || !strings.HasSuffix(out, "imported 5 keys to board /dev/cu.usbmodemB\n") {
		t.Fatalf("stdin to -port: %v %q", err, out)
	}
	if len(c.boards["/dev/cu.usbmodemA"].sets) != 0 || len(c.boards["/dev/cu.usbmodemB"].sets) != 5 {
		t.Fatal("wrong board written")
	}
	// Two boards, no -port; no board: refused, before anything is opened.
	if _, _, err := c.run("", "import", file); err == nil || !strings.Contains(err.Error(), "several boards found") {
		t.Fatalf("two boards: %v", err)
	}
	c = newConfigBench(t, 0)
	if _, _, err := c.run("", "import", file); err == nil || !strings.Contains(err.Error(), "no board found") {
		t.Fatalf("no board: %v", err)
	}

	// To the local store with -local: one write, other lines kept, a
	// warning for the password (and not its value).
	local := filepath.Join(os.Getenv("HOME"), ".gogo", "config")
	os.MkdirAll(filepath.Dir(local), 0o700)
	os.WriteFile(local, []byte("# mine\nnet.ip=10.0.0.1\nkeep=1\n"), 0o600)
	out, errOut, err = c.run("", "-local", "import", file)
	if err != nil || out != wantOut+"imported 5 keys to local store "+local+"\n" {
		t.Fatalf("local import: %v\n%s", err, out)
	}
	if !strings.Contains(errOut, "warning: wifi.password is kept in plain text") || strings.Contains(out+errOut, "hunter2") {
		t.Fatalf("local import stderr %q", errOut)
	}
	data, _ := os.ReadFile(local)
	if string(data) != "# mine\nnet.ip=192.168.1.20\nkeep=1\nwifi.ssid=Home Net\ndemo.url=http://x/?a=b&c=d\ngreeting=  spaced  \nwifi.password=hunter2\n" {
		t.Fatalf("local file %q", data)
	}
	// $GOGO_CONFIG counts as choosing the local store.
	envFile := filepath.Join(t.TempDir(), "cfg")
	t.Setenv("GOGO_CONFIG", envFile)
	if _, _, err := newConfigBench(t, 1).run("", "import", file); err != nil {
		t.Fatal(err)
	}
	if data, _ := os.ReadFile(envFile); !strings.Contains(string(data), "wifi.ssid=Home Net\n") {
		t.Fatalf("GOGO_CONFIG file %q", data)
	}
}

// A bad file writes nothing and names every bad line.
func TestConfigImportErrors(t *testing.T) {
	t.Setenv("GOGO_CONFIG", "")
	cases := []struct{ file, want string }{
		{"Blink=1\n", "f.cfg:1: bad key \"Blink\""},
		{"ok.key=1\nnet.ip=1.2.3\n", "f.cfg:2: bad value for net.ip (ip)"},
		{"net.bogus=1\n", "f.cfg:1: net.bogus is not a key gogo knows"},
		{"a=1\n\nb=2\na=3\n", "f.cfg:4: a is set again (first on line 1)"},
		{"just words\n", "f.cfg:1: want key=value"},
		{"=x\n", "f.cfg:1: bad key \"\""},
		{"a=1\nwifi.password=" + strings.Repeat("p", 65) + "\nx y=2\n", "f.cfg:2: bad value for wifi.password (string): want at most 64 bytes\n  f.cfg:3: bad key \"x y\""},
		{"# nothing\n\n", "f.cfg has no settings"},
		{"", "f.cfg has no settings"},
	}
	for _, tc := range cases {
		dir := t.TempDir()
		file := filepath.Join(dir, "f.cfg")
		os.WriteFile(file, []byte(tc.file), 0o600)
		t.Chdir(dir)
		c := newConfigBench(t, 1)
		out, _, err := c.run("", "import", "f.cfg")
		if err == nil || !strings.Contains(err.Error(), tc.want) {
			t.Errorf("%q: %v, want %q", tc.file, err, tc.want)
		}
		if len(c.opened) != 0 || out != "" {
			t.Errorf("%q: opened %q, out %q", tc.file, c.opened, out)
		}
		if strings.Contains(fmt.Sprint(err), "ppppp") {
			t.Errorf("%q: the value is in the error", tc.file)
		}
	}
	// No final newline: the last line still counts.
	file := filepath.Join(t.TempDir(), "f")
	os.WriteFile(file, []byte("a=1\nb=2"), 0o600)
	c := newConfigBench(t, 1)
	if _, _, err := c.run("", "import", file); err != nil || c.boards[c.ports[0]].store["b"] != "2" {
		t.Errorf("no final newline: %v %q", err, c.boards[c.ports[0]].store)
	}
	if _, _, err := c.run("", "import", filepath.Join(t.TempDir(), "missing")); err == nil || !strings.Contains(err.Error(), "no such file") {
		t.Errorf("missing file: %v", err)
	}
	if _, _, err := c.run("", "import"); err == nil || !strings.Contains(err.Error(), "usage") {
		t.Errorf("no file: %v", err)
	}
	if _, _, err := c.run("", "-n", "list"); err == nil || !strings.Contains(err.Error(), "for import only") {
		t.Errorf("-n list: %v", err)
	}
}

// A board that refuses a key stops the import: the keys before it are
// written, the rest are not, and the error says which.
func TestConfigImportPartial(t *testing.T) {
	t.Setenv("GOGO_CONFIG", "")
	c := newConfigBench(t, 1)
	b := c.boards[c.ports[0]]
	b.failOn = "wifi.password"
	out, errOut, err := c.run("a=1\nwifi.password=hunter2\nb=2\nc=3\n", "import", "-")
	want := "gogo config: import stopped at stdin:2 (wifi.password): board: ESP_ERR_NVS_NOT_ENOUGH_SPACE\n  written: a\n  not written: wifi.password, b, c"
	if err == nil || err.Error() != want {
		t.Fatalf("err %v\nwant %s", err, want)
	}
	if out != "set a\n" || fmt.Sprint(b.sets) != "[a]" {
		t.Fatalf("out %q sets %q", out, b.sets)
	}
	if strings.Contains(out+errOut+err.Error(), "hunter2") {
		t.Fatal("the password is in the output")
	}
}

// -n (-dry-run) checks and names the store but writes nothing.
func TestConfigImportDryRun(t *testing.T) {
	t.Setenv("GOGO_CONFIG", "")
	t.Setenv("HOME", t.TempDir())
	for _, flag := range []string{"-n", "--dry-run"} {
		c := newConfigBench(t, 1)
		out, errOut, err := c.run(importFile, "import", "-", flag)
		if err != nil || out != "would set wifi.ssid\nwould set net.ip\nwould set demo.url\nwould set greeting\nwould set wifi.password (set)\nwould import 5 keys to board /dev/cu.usbmodemA (dry run: nothing written)\n" {
			t.Fatalf("%s: %v\n%s", flag, err, out)
		}
		if len(c.opened) != 0 || strings.Contains(out+errOut, "hunter2") {
			t.Fatalf("%s: opened %q", flag, c.opened)
		}
		out, _, err = c.run(importFile, flag, "-local", "import", "-")
		if err != nil || !strings.HasSuffix(out, "to local store "+filepath.Join(os.Getenv("HOME"), ".gogo", "config")+" (dry run: nothing written)\n") {
			t.Fatalf("%s -local: %v %q", flag, err, out)
		}
		if _, err := os.Stat(filepath.Join(os.Getenv("HOME"), ".gogo")); err == nil {
			t.Fatalf("%s -local wrote the store", flag)
		}
		if _, _, err := c.run("x y=1\n", "import", "-", flag); err == nil || !strings.Contains(err.Error(), "stdin:1: bad key") {
			t.Fatalf("%s: bad file: %v", flag, err)
		}
	}
}

// export prints an import file: secrets as comments without values, keys
// import would refuse as comments; it round-trips through import.
func TestConfigExport(t *testing.T) {
	t.Setenv("GOGO_CONFIG", "")
	c := newConfigBench(t, 2)
	src := c.boards["/dev/cu.usbmodemA"]
	for k, v := range map[string]string{"wifi.ssid": "Home Net", "wifi.password": "hunter2", "node.token": "tok3n", "demo.url": "http://x/?a=b", "Old-Key": "1", "pad": " x "} {
		src.store[k] = v
	}
	out, _, err := c.run("", "export", "-port", "/dev/cu.usbmodemA")
	want := "# gogo config export from the board /dev/cu.usbmodemA\n" +
		"# Old-Key=1 (not importable: bad key \"Old-Key\" (1-15 characters of a-z 0-9 . _))\n" +
		"demo.url=http://x/?a=b\n# node.token=(set, not exported)\npad= x \n" +
		"# wifi.password=(set, not exported)\nwifi.ssid=Home Net\n"
	if err != nil || out != want {
		t.Fatalf("export: %v\n%s\nwant\n%s", err, out, want)
	}
	if strings.Contains(out, "hunter2") || strings.Contains(out, "tok3n") {
		t.Fatal("a secret was exported")
	}
	if _, _, err := c.run(out, "import", "-", "-port", "/dev/cu.usbmodemB"); err != nil {
		t.Fatal(err)
	}
	if got := fmt.Sprint(c.boards["/dev/cu.usbmodemB"].store); got != "map[demo.url:http://x/?a=b pad: x  wifi.ssid:Home Net]" {
		t.Fatalf("round trip: %s", got)
	}
	// The local store, first line of a key winning.
	path := filepath.Join(t.TempDir(), "cfg")
	os.WriteFile(path, []byte("# mine\nb=2\na=1\nb=dup\nnode.token=tok3n\n"), 0o600)
	t.Setenv("GOGO_CONFIG", path)
	out, _, err = c.run("", "export")
	if err != nil || out != "# gogo config export from the local store "+path+"\na=1\nb=2\n# node.token=(set, not exported)\n" {
		t.Fatalf("local export: %v\n%s", err, out)
	}
}
