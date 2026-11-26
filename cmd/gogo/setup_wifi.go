package main

// gogo setup, the last two steps: Wi-Fi (a scan on the board, a choice,
// the password read without echo and written with the board's config
// command, as `gogo config set` does) and a first program (flashed like
// `gogo flash`, then watched on the console until it prints).
//
// The Wi-Fi password is only ever sent to the board: it is never printed,
// logged, cached or put on a command line. The board never sends it back.
// It is not wiped from this process's memory (Go strings cannot be); the
// process ends right after setup.

import (
	"bytes"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"sync"
	"time"
)

// serialSupported reports whether openSerial works on goos
// (monitor_tty.go).
func serialSupported(goos string) bool { return goos == "darwin" || goos == "linux" }

// network is one line of the board's Wi-Fi scan.
type network struct {
	ssid   string
	detail string // "ch 6 -61 dBm"
	open   bool
}

// The board's scan lines (platform/esp32s3/main/wifi.c): the SSID padded
// to 32 columns, then "ch N RSSI dBm open|secured".
var scanLineRe = regexp.MustCompile(`^(.*?)\s+(ch\s+\d+\s+-?\d+ dBm)\s+(open|secured)$`)

func parseScan(text string) []network {
	var nets []network
	seen := map[string]bool{}
	for _, line := range strings.Split(text, "\n") {
		m := scanLineRe.FindStringSubmatch(strings.TrimRight(line, "\r"))
		if m == nil || m[1] == "" || seen[m[1]] {
			continue // hidden networks have no name; the strongest of a name comes first
		}
		seen[m[1]] = true
		nets = append(nets, network{ssid: m[1], detail: strings.Join(strings.Fields(m[2]), " "), open: m[3] == "open"})
	}
	return nets
}

// openBoard opens the board's serial console, waiting for the port to
// come back after the reset that ends each esptool command.
func (s *setup) openBoard() (*console, error) {
	var err error
	for i := 0; i < 20; i++ {
		var rw io.ReadWriteCloser
		if rw, err = s.env.openPort(s.port); err == nil {
			return newConsole(rw), nil
		}
		time.Sleep(s.env.retryDelay)
	}
	return nil, err
}

// console owns the board's serial port while gogo setup talks to it. One
// goroutine reads the port; each exchange (a config command, a watch)
// reads through its own view. boardConfig and capture leave a reader
// goroutine behind when they return; through a closed view it gets EOF
// instead of output meant for the next exchange.
type console struct {
	rw   io.ReadWriteCloser
	mu   sync.Mutex
	cond *sync.Cond
	buf  []byte
	err  error
}

func newConsole(rw io.ReadWriteCloser) *console {
	c := &console{rw: rw}
	c.cond = sync.NewCond(&c.mu)
	go func() {
		b := make([]byte, 512)
		for {
			n, err := rw.Read(b)
			c.mu.Lock()
			c.buf = append(c.buf, b[:n]...)
			if err != nil {
				c.err = err
			}
			c.cond.Broadcast()
			c.mu.Unlock()
			if err != nil {
				return
			}
		}
	}()
	return c
}

func (c *console) Close() error { return c.rw.Close() }

// consoleView is one exchange's reader. With endOnReply it ends itself
// right after the reply frame of a config command (\x02ok or \x02err up
// to its newline), leaving everything after it for the next exchange.
type consoleView struct {
	c          *console
	closed     bool
	endOnReply bool
	inFrame    bool
	tag        []byte
}

func (v *consoleView) Write(p []byte) (int, error) { return v.c.rw.Write(p) }

func (v *consoleView) Read(p []byte) (int, error) {
	c := v.c
	c.mu.Lock()
	defer c.mu.Unlock()
	for len(c.buf) == 0 && !v.closed && c.err == nil {
		c.cond.Wait()
	}
	if v.closed {
		return 0, io.EOF
	}
	if len(c.buf) == 0 {
		return 0, c.err
	}
	n := 0
	for n < len(p) && n < len(c.buf) && !v.closed {
		ch := c.buf[n]
		p[n] = ch
		n++
		if !v.endOnReply {
			continue
		}
		switch {
		case ch == 0x02:
			v.inFrame, v.tag = true, v.tag[:0]
		case !v.inFrame:
		case ch == '\n':
			v.inFrame = false
			if bytes.HasPrefix(v.tag, []byte("ok")) || bytes.HasPrefix(v.tag, []byte("err")) {
				v.closed = true
			}
		case len(v.tag) < 3:
			v.tag = append(v.tag, ch)
		}
	}
	c.buf = c.buf[n:]
	return n, nil
}

func (v *consoleView) close() {
	v.c.mu.Lock()
	v.closed = true
	v.c.cond.Broadcast()
	v.c.mu.Unlock()
}

// config runs one `gogo config` command on the board (boardConfig).
func (c *console) config(op string, args []string, out io.Writer, timeout time.Duration) error {
	v := &consoleView{c: c, endOnReply: true}
	defer v.close()
	return boardConfig(v, op, args, out, timeout)
}

// watch reads the console until f returns true for a line or timeout
// passes; it reports whether f ended it.
func (c *console) watch(timeout time.Duration, f func(string) bool) bool {
	v := &consoleView{c: c}
	defer v.close()
	return capture(v, &lineWatch{f: f}, timeout, "") == errWatchDone
}

// setupWifi is step 5.
func (s *setup) setupWifi() error {
	const later = "set it later with `gogo config set wifi.ssid <name>` and `gogo config set wifi.password -`"
	switch {
	case s.o.skipWifi:
		fmt.Fprintln(s.env.out, "  skipped (-skip-wifi); Wi-Fi settings already on the board are kept")
		return nil
	case s.o.yes:
		fmt.Fprintf(s.env.out, "  skipped (-yes); %s\n", later)
		return nil
	case !serialSupported(s.env.goos):
		fmt.Fprintf(s.env.out, "  skipped: gogo cannot use the serial console on %s; set Wi-Fi from macOS or Linux with gogo config\n", s.env.goos)
		return nil
	}
	ok, err := s.confirm("  Set up Wi-Fi now?")
	if err != nil {
		return err
	}
	if !ok {
		fmt.Fprintf(s.env.out, "  skipped; %s\n", later)
		return nil
	}
	rw, err := s.openBoard()
	if err != nil {
		fmt.Fprintf(s.env.out, "  cannot open %s (%v); %s\n", s.port, err, later)
		return nil
	}
	defer rw.Close()
	fmt.Fprintln(s.env.out, "  scanning for networks on the board...")
	var nets []network
	for tries := 0; tries < 4; tries++ {
		var buf bytes.Buffer
		err = rw.config("scan", nil, &buf, 25*time.Second)
		if err == nil {
			nets = parseScan(buf.String())
			break
		}
		time.Sleep(4 * s.env.retryDelay) // the board is still starting Wi-Fi
	}
	if err != nil {
		fmt.Fprintf(s.env.out, "  scan failed (%v); type the network name instead\n", err)
	}
	var options []string
	for _, n := range nets {
		options = append(options, fmt.Sprintf("%-32s %s%s", n.ssid, n.detail, map[bool]string{true: "  open", false: ""}[n.open]))
	}
	options = append(options, "another network (type its name)")
	n, err := s.choose("Which network?", options, 1)
	if err != nil {
		return err
	}
	var net network
	if n <= len(nets) {
		net = nets[n-1]
	} else {
		name, err := s.ask("  Network name:", "")
		if err != nil {
			return err
		}
		if a, err := s.ask("  Does it have a password? [Y/n]", "y"); err != nil {
			return err
		} else {
			net = network{ssid: name, open: !strings.HasPrefix(strings.ToLower(a), "y")}
		}
	}
	if net.ssid == "" || len(net.ssid) > 32 || checkValue(net.ssid) != nil {
		return setupFail("pick a network from the list or type its exact name (1 to 32 bytes)", "bad network name")
	}
	var password string
	if !net.open {
		if password, err = s.readPassword(); err != nil {
			return err
		}
	}
	if s.o.dryRun {
		fmt.Fprintf(s.env.out, "  would set wifi.ssid to %q", net.ssid)
		if net.open {
			fmt.Fprintln(s.env.out, " and remove wifi.password")
		} else {
			fmt.Fprintln(s.env.out, " and wifi.password (not shown)")
		}
		return nil
	}
	// The password first, so the reconnect that setting wifi.ssid starts
	// already uses it.
	if net.open {
		if err := rw.config("rm", []string{"wifi.password"}, io.Discard, 5*time.Second); err != nil && !strings.Contains(err.Error(), "not set") {
			return setupFail(later, "cannot write the Wi-Fi settings: %v", err)
		}
	} else if err := rw.config("set", []string{"wifi.password", password}, io.Discard, 5*time.Second); err != nil {
		return setupFail(later, "cannot write the Wi-Fi password: %v", err)
	}
	if err := rw.config("set", []string{"wifi.ssid", net.ssid}, io.Discard, 5*time.Second); err != nil {
		return setupFail(later, "cannot write the Wi-Fi network name: %v", err)
	}
	fmt.Fprintf(s.env.out, "  saved on the board: wifi.ssid=%s, wifi.password=(set)\n", net.ssid)
	fmt.Fprintln(s.env.out, "  waiting for the board to join...")
	var joined, failed bool
	rw.watch(40*time.Second, func(line string) bool {
		switch {
		case strings.HasPrefix(line, "gogo: wifi connected"):
			joined = true
		case strings.Contains(line, "wrong password"), strings.Contains(line, "network not found"):
			failed = true
		default:
			return false
		}
		fmt.Fprintf(s.env.out, "  | %s\n", line)
		return true
	})
	switch {
	case joined:
		fmt.Fprintln(s.env.out, "  Wi-Fi works")
	case failed:
		fmt.Fprintf(s.env.out, "  the board could not join %q; check the password and %s\n", net.ssid, later)
	default:
		fmt.Fprintf(s.env.out, "  no connection yet (the board keeps trying); check with `gogo monitor -timeout 20`\n")
	}
	return nil
}

// readPassword reads the Wi-Fi password from stdin with echo off.
func (s *setup) readPassword() (string, error) {
	for tries := 0; tries < 3; tries++ {
		restore := noEcho(s.env.stdin, s.env.out, "  Wi-Fi password (not shown)")
		line, err := s.env.in.ReadString('\n')
		restore()
		if err != nil && (err != io.EOF || line == "") {
			return "", setupFail("run gogo setup in a terminal, or pass -skip-wifi", "no password on stdin")
		}
		pw := strings.TrimRight(line, "\r\n")
		switch {
		case len(pw) < 8 || len(pw) > 63:
			fmt.Fprintln(s.env.out, "  a Wi-Fi (WPA) password has 8 to 63 characters; try again")
		case checkValue(pw) != nil:
			fmt.Fprintln(s.env.out, "  the password cannot contain control characters; try again")
		default:
			return pw, nil
		}
	}
	return "", setupFail("check the password and run gogo setup again", "no valid password")
}

// errWatchDone ends a watch from its line callback.
var errWatchDone = errors.New("done")

// lineWatch is an io.Writer that hands whole console lines to f; f
// returns true to end the watch.
type lineWatch struct {
	buf []byte
	f   func(string) bool
}

func (w *lineWatch) Write(p []byte) (int, error) {
	w.buf = append(w.buf, p...)
	for {
		i := bytes.IndexByte(w.buf, '\n')
		if i < 0 {
			return len(p), nil
		}
		line := strings.TrimRight(string(w.buf[:i]), "\r")
		w.buf = w.buf[i+1:]
		if w.f(line) {
			return len(p), errWatchDone
		}
	}
}

// firstProgram is step 6: flash the hello program and watch it print.
func (s *setup) firstProgram() error {
	tmp, err := os.MkdirTemp("", "gogo-setup-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(tmp)
	file := filepath.Join(tmp, "hello"+imageExt)
	if err := writeHello(s.board, file); err != nil {
		return err
	}
	img, err := os.ReadFile(file)
	if err != nil {
		return err
	}
	fmt.Fprintf(s.env.out, "  hello program: %d bytes, to the image partition at %s\n", len(img), partOffset)
	run := func(_ string, args []string) error { return s.runEsptool(args, true) }
	if err := writeImage(run, s.esptool, s.port, img, []string{"--chip", "esp32s3", "--after", "watchdog-reset"}); err != nil {
		return setupFail("check the cable and that nothing else uses the port, then run gogo setup again", "flashing the program failed: %v", err)
	}
	if s.o.dryRun {
		return nil
	}
	if !serialSupported(s.env.goos) {
		fmt.Fprintf(s.env.out, "  look at the board's screen for the greeting (gogo cannot read the serial console on %s)\n", s.env.goos)
		return nil
	}
	rw, err := s.openBoard()
	if err != nil {
		return setupFail("watch the board's screen, or run `gogo monitor -timeout 10` on a supported OS", "cannot open %s to watch the program: %v", s.port, err)
	}
	defer rw.Close()
	fmt.Fprintln(s.env.out, "  watching the board's console...")
	var bad string
	ok := rw.watch(40*time.Second, func(line string) bool {
		switch {
		case strings.HasPrefix(line, "gogo: bad image"), strings.HasPrefix(line, "gogo: image needs"):
			bad = line
		case strings.HasPrefix(line, "hello from gogo"):
		case strings.HasPrefix(line, "gogo: wifi connected"), strings.HasPrefix(line, "gogo: wifi not configured"), strings.HasPrefix(line, "gogo: wifi:"):
			fmt.Fprintf(s.env.out, "  | %s\n", line)
			return false
		case strings.HasPrefix(line, "gogo ") && strings.Contains(line, ": image "):
			fmt.Fprintf(s.env.out, "  | %s\n", line) // the boot line
			return false
		default:
			return false
		}
		fmt.Fprintf(s.env.out, "  | %s\n", line)
		return true
	})
	if bad != "" {
		return setupFail("use the gogo command from the same release as the firmware (`gogo version`), or rebuild the firmware",
			"the firmware refused the program: %s", bad)
	}
	if !ok {
		return setupFail("look at the board's screen, and run `gogo monitor -timeout 10` to see what it prints", "the program did not print within 40 seconds")
	}
	return nil
}

// nextSteps is printed at the end of a successful setup.
func (s *setup) nextSteps() {
	monitor := "gogo monitor -reset -timeout 10"
	if s.board == "cardputer" {
		monitor = "gogo monitor -timeout 10" // no -reset: the Cardputer ADV does not come back from its RTS reset
	}
	fmt.Fprintf(s.env.out, "\nYour %s runs gogo. Next:\n", setupBoardTitles[s.board])
	fmt.Fprintf(s.env.out, "  gogo build myapp -o myapp%s    compile a program (a directory with package main)\n", imageExt)
	fmt.Fprintf(s.env.out, "  gogo flash myapp%s             put it on the board (the firmware stays)\n", imageExt)
	if serialSupported(s.env.goos) {
		fmt.Fprintf(s.env.out, "  %-33s watch what it prints\n", monitor)
		fmt.Fprintln(s.env.out, "  gogo config list                  see the board's settings (Wi-Fi and more)")
	}
	fmt.Fprintln(s.env.out, "The gogo book, docs/book in the gogo repository, starts at chapter 2 (Getting Started):")
	fmt.Fprintln(s.env.out, "  https://github.com/c4pt0r/gogo/blob/main/docs/book/02-getting-started.md")
}
