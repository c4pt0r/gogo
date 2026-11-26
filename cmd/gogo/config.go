package main

import (
	"bufio"
	"bytes"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"slices"
	"sort"
	"strconv"
	"strings"
	"syscall"
	"time"
)

// `gogo config set|get|list|rm` edits the store that
// sys.Config reads. On this computer: $GOGO_CONFIG, else ~/.gogo/config,
// one key=value per line (the first line of a key wins). On the board:
// its NVS store, over the serial console (boardConfig; `scan` lists the
// Wi-Fi networks the board sees, `stats` its net stack's counters). See configMain for which one is used.
//
// set checks the key and the value against the key table (cfgKeys): a key
// is 1-15 bytes of [a-z0-9._] (15 is the ESP32's NVS limit, so a
// configuration means the same on every platform); a key under a
// platform prefix (cfgReserved) must be one of the table's, and its value
// must parse as the key's type, as the firmware or library that reads it
// parses it. Values are at most configMaxValue bytes without newlines.
// get and rm also take the older key spelling (A-Z and "-" too), so keys
// set before the table can still be read and removed. `get` and `list`
// never print the Wi-Fi password, only whether it is set.

const configUsage = "usage: gogo config [-port P | -local] set <key> <value|-> | get <key> | list | rm <key> | keys | scan | stats"

const (
	configMaxKey   = 15
	configMaxValue = 1000
)

// configSecret is shown as "(set)".
const configSecret = "wifi.password"

func configPath() (string, error) {
	if p := os.Getenv("GOGO_CONFIG"); p != "" {
		return p, nil
	}
	home, err := os.UserHomeDir()
	if err != nil {
		return "", err
	}
	return filepath.Join(home, ".gogo", "config"), nil
}

// validKey reports whether k is a key get and rm accept: 1-15 bytes of
// [A-Za-z0-9._-] (what the board's store accepts).
func validKey(k string) bool {
	if k == "" || len(k) > configMaxKey {
		return false
	}
	for i := 0; i < len(k); i++ {
		c := k[i]
		if !(c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9' || c == '.' || c == '_' || c == '-') {
			return false
		}
	}
	return true
}

// newKeyOK reports whether k may be set: 1-15 bytes of [a-z0-9._].
func newKeyOK(k string) bool {
	if k == "" || len(k) > configMaxKey {
		return false
	}
	for i := 0; i < len(k); i++ {
		c := k[i]
		if !(c >= 'a' && c <= 'z' || c >= '0' && c <= '9' || c == '.' || c == '_') {
			return false
		}
	}
	return true
}

// cfgType is how a value is checked on set.
type cfgType int

const (
	cfgString cfgType = iota // any value
	cfgSize                  // bytes: digits with an optional K or M suffix (k, m too), at most 4 GB - 1
	cfgIP                    // a dotted IPv4 address
	cfgBool                  // yes or no
	cfgInt                   // a decimal integer (31-bit, as a gogo int)
)

var cfgTypeNames = [...]string{"string", "size", "ip", "bool", "int"}

// cfgKey is one key the platform or a gogo library reads.
type cfgKey struct {
	name     string
	typ      cfgType
	platform bool     // read by the firmware or the VM, not a program (wifi.* is hidden from programs)
	maxLen   int      // longest value; 0: configMaxValue
	values   []string // if set, the only values allowed
	about    string
}

// cfgKeys are the keys gogo itself reads (book chapter 11.3). Keep the
// book's table in step.
var cfgKeys = []cfgKey{
	{name: "wifi.ssid", typ: cfgString, platform: true, maxLen: 32, about: "the Wi-Fi network"},
	{name: "wifi.password", typ: cfgString, platform: true, maxLen: 64, about: "its password (never printed)"},
	{name: "net.ip", typ: cfgIP, about: "a static address instead of DHCP (gogo/dhcp)"},
	{name: "net.mask", typ: cfgIP, about: "its netmask (default 255.255.255.0)"},
	{name: "net.gw", typ: cfgIP, about: "its gateway"},
	{name: "net.dns", typ: cfgIP, about: "its DNS server"},
	{name: "fs.flash.size", typ: cfgSize, platform: true, about: "the size of /flash"},
	{name: "fs.flash.format", typ: cfgString, platform: true, values: []string{"yes"}, about: "yes: format /flash at the next boot"},
	{name: "code.mem", typ: cfgSize, platform: true, about: "the budget for loaded module code"},
}

// cfgReserved are the prefixes of the platform's and gogo's own keys: a
// key under one must be in cfgKeys.
var cfgReserved = []string{"wifi.", "net.", "fs.", "code.", "gogo."}

func findCfgKey(k string) (cfgKey, bool) {
	for _, c := range cfgKeys {
		if c.name == k {
			return c, true
		}
	}
	return cfgKey{}, false
}

// writeCfgKeys prints the key table (`gogo config keys`).
func writeCfgKeys(out io.Writer) {
	for _, c := range cfgKeys {
		owner := "gogo library"
		if c.platform {
			owner = "platform"
		}
		typ := cfgTypeNames[c.typ]
		if c.values != nil {
			typ = strings.Join(c.values, "|")
		}
		fmt.Fprintf(out, "%-15s  %-6s  %-12s  %s\n", c.name, typ, owner, c.about)
	}
	fmt.Fprintf(out, "reserved prefixes: %s (other keys are yours)\n", strings.Join(cfgReserved, " "))
}

// checkSetKey reports why key cannot be set, or nil.
func checkSetKey(key string) error {
	if !newKeyOK(key) {
		return fmt.Errorf("gogo config: bad key %q (1-%d characters of a-z 0-9 . _)", key, configMaxKey)
	}
	if _, ok := findCfgKey(key); !ok {
		for _, p := range cfgReserved {
			if strings.HasPrefix(key, p) {
				return fmt.Errorf("gogo config: %s is not a key gogo knows, and %s* keys belong to gogo (pick another prefix for your own)", key, p)
			}
		}
	}
	return nil
}

// checkSetting reports why `set key value` is refused, or nil.
func checkSetting(key, value string) error {
	if err := checkSetKey(key); err != nil {
		return err
	}
	if err := checkValue(value); err != nil {
		return err
	}
	c, ok := findCfgKey(key)
	if !ok {
		return nil
	}
	bad := func(want string) error {
		return fmt.Errorf("gogo config: bad value for %s (%s): want %s", key, cfgTypeNames[c.typ], want)
	}
	if c.maxLen > 0 && len(value) > c.maxLen {
		return bad(fmt.Sprintf("at most %d bytes", c.maxLen))
	}
	if c.values != nil && !slices.Contains(c.values, value) {
		return bad(strings.Join(c.values, " or "))
	}
	switch c.typ {
	case cfgSize:
		if !sizeOK(value) {
			return bad("bytes, with an optional K or M suffix, like 65536, 64K or 1M")
		}
	case cfgIP:
		if !ipOK(value) {
			return bad("a dotted IPv4 address like 192.168.1.20")
		}
	case cfgBool:
		if value != "yes" && value != "no" {
			return bad("yes or no")
		}
	case cfgInt:
		if n, err := strconv.ParseInt(value, 10, 32); err != nil || n < -(1<<30) || n >= 1<<30 {
			return bad("a decimal integer")
		}
	}
	return nil
}

// sizeOK: digits, an optional K or M (either case), at most 2^32-1 bytes,
// as the firmware and the VM parse it (fs_lfs.c, module.c).
func sizeOK(v string) bool {
	i := 0
	for i < len(v) && v[i] >= '0' && v[i] <= '9' {
		i++
	}
	if i == 0 || i > 10 {
		return false
	}
	n, _ := strconv.ParseUint(v[:i], 10, 64)
	switch v[i:] {
	case "":
	case "K", "k":
		n <<= 10
	case "M", "m":
		n <<= 20
	default:
		return false
	}
	return n <= 0xFFFFFFFF
}

// ipOK: four decimal parts 0..255 of 1 to 3 digits, as gogo/dhcp parses it.
func ipOK(v string) bool {
	parts := strings.Split(v, ".")
	if len(parts) != 4 {
		return false
	}
	for _, p := range parts {
		if len(p) == 0 || len(p) > 3 {
			return false
		}
		n, err := strconv.Atoi(p)
		if err != nil || n < 0 || n > 255 || p[0] == '+' || p[0] == '-' {
			return false
		}
	}
	return true
}

// readConfig returns the store's lines (none if it does not exist).
func readConfig(path string) ([]string, error) {
	data, err := os.ReadFile(path)
	if errors.Is(err, os.ErrNotExist) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	lines := strings.Split(strings.TrimSuffix(string(data), "\n"), "\n")
	if len(data) == 0 {
		lines = nil
	}
	return lines, nil
}

// lookup returns the value of the first line for key.
func lookup(lines []string, key string) (string, bool) {
	for _, l := range lines {
		if k, v, ok := strings.Cut(strings.TrimSuffix(l, "\r"), "="); ok && k == key {
			return v, true
		}
	}
	return "", false
}

// writeConfig replaces the store atomically, readable by the user only.
func writeConfig(path string, lines []string) error {
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		return err
	}
	f, err := os.CreateTemp(filepath.Dir(path), ".config-*")
	if err != nil {
		return err
	}
	var b strings.Builder
	for _, l := range lines {
		b.WriteString(l + "\n")
	}
	_, err = f.WriteString(b.String())
	if cerr := f.Close(); err == nil {
		err = cerr
	}
	if err == nil {
		err = os.Chmod(f.Name(), 0o600)
	}
	if err == nil {
		err = os.Rename(f.Name(), path)
	}
	if err != nil {
		os.Remove(f.Name())
	}
	return err
}

// checkValue reports why v cannot be a value in the store, or nil. The
// board's frame cannot carry newlines, NUL or STX.
func checkValue(v string) error {
	if len(v) > configMaxValue || strings.ContainsAny(v, "\n\r\x00\x02") {
		return fmt.Errorf("gogo config: bad value (at most %d bytes, no newlines)", configMaxValue)
	}
	return nil
}

func shown(key, value string) string {
	if key == configSecret {
		return "(set)"
	}
	return value
}

func configCmd(args []string, out io.Writer) error {
	return configMain(args, out, os.Stdin, os.Stderr, filepath.Glob)
}

// configMain: `gogo config [-port P | -local] <op> ...`. The store is the
// board's (over its serial console) with -port, the local file with
// -local; otherwise the local file if $GOGO_CONFIG is set, else the board
// if exactly one is connected, else the local file. A value of "-" is read
// as one line from stdin (keeps a password out of the shell history).
func configMain(args []string, out io.Writer, stdin io.Reader, errOut io.Writer, glob func(string) ([]string, error)) error {
	fs := newFlags("config")
	port := fs.String("port", "", "")
	local := fs.Bool("local", false, "")
	// set's value is taken as it is, even if it starts with "-".
	args, err := parseFlagsRaw(fs, args, configUsage, func(pos []string) bool { return len(pos) == 2 && pos[0] == "set" })
	if err != nil {
		return err
	}
	if *port != "" && *local {
		return usageError("config", "-port and -local do not combine", configUsage)
	}
	if len(args) == 0 {
		return usageError("config", "missing operation", configUsage)
	}
	op, args := args[0], args[1:]
	want := map[string]int{"set": 2, "get": 1, "list": 0, "rm": 1, "keys": 0, "scan": 0, "stats": 0}
	if n, ok := want[op]; !ok {
		return usageError("config", fmt.Sprintf("unknown operation %q", op), configUsage)
	} else if len(args) != n {
		return usageError("config", fmt.Sprintf("%s takes %d argument(s)", op, n), configUsage)
	}
	if op == "set" {
		if err := checkSetKey(args[0]); err != nil {
			return err
		}
	} else if len(args) > 0 && !validKey(args[0]) {
		return fmt.Errorf("gogo config: bad key %q (1-%d characters of a-z 0-9 . _)", args[0], configMaxKey)
	}
	if op == "keys" {
		writeCfgKeys(out)
		return nil
	}

	if op == "set" && args[1] == "-" {
		restore := noEcho(stdin, errOut, args[0])
		v, err := bufio.NewReader(stdin).ReadString('\n')
		restore()
		if err != nil && err != io.EOF {
			return err
		}
		args[1] = strings.TrimRight(v, "\r\n")
	}
	if op == "set" {
		if err := checkSetting(args[0], args[1]); err != nil {
			return err
		}
	}
	if *port == "" && !*local && os.Getenv("GOGO_CONFIG") == "" {
		if ports, err := glob(portGlob); err == nil && len(ports) == 1 {
			*port = ports[0]
			fmt.Fprintf(errOut, "gogo config: board on %s (-local for this computer's store)\n", *port)
		}
	}
	if *port != "" {
		f, err := openSerial(*port)
		if err != nil {
			return err
		}
		defer f.Close()
		timeout := 5 * time.Second
		if op == "scan" {
			timeout = 20 * time.Second
		}
		return boardConfig(f, op, args, out, timeout)
	}
	if op == "scan" || op == "stats" {
		return fmt.Errorf("gogo config: %s needs a board (-port)", op)
	}
	return fileConfig(op, args, out)
}

// fileConfig runs op on the local store.
func fileConfig(op string, args []string, out io.Writer) error {
	path, err := configPath()
	if err != nil {
		return err
	}
	lines, err := readConfig(path)
	if err != nil {
		return err
	}
	switch op {
	case "get":
		v, ok := lookup(lines, args[0])
		if !ok {
			return fmt.Errorf("gogo config: %s is not set", args[0])
		}
		fmt.Fprintln(out, shown(args[0], v))
	case "list":
		var kvs []string
		seen := map[string]bool{}
		for _, l := range lines {
			if k, v, ok := strings.Cut(strings.TrimSuffix(l, "\r"), "="); ok && validKey(k) && !seen[k] {
				seen[k] = true
				kvs = append(kvs, k+"="+shown(k, v))
			}
		}
		sort.Strings(kvs)
		for _, kv := range kvs {
			fmt.Fprintln(out, kv)
		}
	case "set", "rm":
		key := args[0]
		var kept []string
		found := false
		for _, l := range lines {
			if k, _, ok := strings.Cut(strings.TrimSuffix(l, "\r"), "="); ok && k == key {
				if op == "set" && !found {
					kept = append(kept, key+"="+args[1])
				}
				found = true
				continue
			}
			kept = append(kept, l)
		}
		if op == "rm" && !found {
			return fmt.Errorf("gogo config: %s is not set", key)
		}
		if op == "set" {
			v := args[1]
			if len(v) > configMaxValue || strings.ContainsAny(v, "\n\r\x00") {
				return fmt.Errorf("gogo config: bad value (at most %d bytes, no newlines)", configMaxValue)
			}
			if !found {
				kept = append(kept, key+"="+v)
			}
		}
		return writeConfig(path, kept)
	}
	return nil
}

// boardConn talks to the board's console handler
// (platform/esp32s3/main/config_esp.c) on rw: one reader for the whole
// session, so several requests can follow each other on one port.
type boardConn struct {
	rw    io.ReadWriter
	lines chan string
	done  chan error
	quit  chan struct{}
}

func newBoardConn(rw io.ReadWriter) *boardConn {
	c := &boardConn{rw: rw, lines: make(chan string), done: make(chan error, 1), quit: make(chan struct{})}
	go func() {
		var buf []byte
		in := false
		b := make([]byte, 512)
		for {
			n, err := rw.Read(b)
			for _, ch := range b[:n] {
				switch {
				case ch == 0x02:
					in, buf = true, buf[:0]
				case !in:
				case ch == '\n':
					in = false
					select {
					case c.lines <- string(bytes.TrimSuffix(buf, []byte("\r"))):
					case <-c.quit:
						return
					}
				default:
					buf = append(buf, ch)
				}
			}
			if err != nil {
				c.done <- err
				return
			}
		}
	}()
	return c
}

// close stops the reader (once its pending Read returns).
func (c *boardConn) close() { close(c.quit) }

// errBoard is the board's own refusal: `\x02err <reason>`.
type errBoard struct{ reason string }

func (e errBoard) Error() string { return "board: " + e.reason }

// request sends one command frame,
//
//	\x02gogo-config <op> [<arg>...]\n
//
// and reads the reply: `\x02kv <text>` lines (kvs), then `\x02ok[ <text>]`
// (text) or `\x02err <reason>` (errBoard). Everything else on the
// console (program output) is skipped.
func (c *boardConn) request(op string, args []string, timeout time.Duration) (kvs []string, text string, err error) {
	frame := "\x02gogo-config " + op
	for _, a := range args {
		frame += " " + a
	}
	if _, err := io.WriteString(c.rw, frame+"\n"); err != nil {
		return nil, "", err
	}
	timer := time.NewTimer(timeout)
	defer timer.Stop()
	for {
		select {
		case l := <-c.lines:
			tag, t, _ := strings.Cut(l, " ")
			switch tag {
			case "kv":
				kvs = append(kvs, t)
			case "ok":
				return kvs, t, nil
			case "err":
				return kvs, "", errBoard{t}
			}
		case err := <-c.done:
			if err == nil || err == io.EOF {
				err = errors.New("connection closed")
			}
			c.done <- err // later requests see it too
			return kvs, "", fmt.Errorf("board: %v", err)
		case <-timer.C:
			return kvs, "", errors.New("no answer from the board (is gogo firmware running? another program using the port?)")
		}
	}
}

// boardConfig runs one config operation on the board and prints its
// result. The board never sends the Wi-Fi password back.
func boardConfig(rw io.ReadWriter, op string, args []string, out io.Writer, timeout time.Duration) error {
	c := newBoardConn(rw)
	defer c.close()
	kvs, text, err := c.request(op, args, timeout)
	var be errBoard
	if errors.As(err, &be) && be.reason == "not set" && len(args) > 0 {
		return fmt.Errorf("gogo config: %s is not set", args[0])
	}
	if err != nil {
		return fmt.Errorf("gogo config: %v", err)
	}
	if op == "list" {
		sort.Strings(kvs)
	}
	for _, kv := range kvs {
		fmt.Fprintln(out, kv)
	}
	if op == "get" {
		fmt.Fprintln(out, text)
	}
	return nil
}

// noEcho turns terminal echo off while a value is typed on stdin, if
// stdin is a terminal (stty via os/exec; stdlib only), and prompts for it.
// The returned function restores echo.
func noEcho(stdin io.Reader, errOut io.Writer, key string) func() {
	f, ok := stdin.(*os.File)
	if !ok {
		return func() {}
	}
	if fi, err := f.Stat(); err != nil || fi.Mode()&os.ModeCharDevice == 0 {
		return func() {}
	}
	stty := func(arg string) error {
		c := exec.Command("stty", arg)
		c.Stdin = f
		return c.Run()
	}
	fmt.Fprintf(errOut, "%s: ", key)
	if stty("-echo") != nil {
		fmt.Fprint(errOut, "(cannot turn echo off: what you type will show) ")
		return func() {}
	}
	// Ctrl-C (or a kill) at the prompt must not leave the terminal
	// without echo: restore it, then exit as the signal would have.
	sigs := make(chan os.Signal, 1)
	done := make(chan struct{})
	signal.Notify(sigs, os.Interrupt, syscall.SIGTERM, syscall.SIGHUP)
	go func() {
		select {
		case <-sigs:
			stty("echo")
			fmt.Fprintln(errOut)
			os.Exit(130)
		case <-done:
		}
	}()
	return func() {
		signal.Stop(sigs)
		close(done)
		stty("echo")
		fmt.Fprintln(errOut)
	}
}
