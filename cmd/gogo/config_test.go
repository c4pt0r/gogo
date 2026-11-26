package main

import (
	"bytes"
	"io"
	"os"
	"path/filepath"
	"strings"
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
	// No GOGO_CONFIG, one board: it is used (and opening this one fails).
	if err := configMain([]string{"list"}, &out, nil, &errOut, board); err == nil || !strings.Contains(errOut.String(), "board on /dev/cu.usbmodem-none") {
		t.Fatalf("auto board: %v %q", err, errOut.String())
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
		{"code.mem", "4194304K", "K or M"},
		{"code.mem", "", "K or M"},
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
