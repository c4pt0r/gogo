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
	"sort"
	"strings"
	"syscall"
	"time"
)

// `gogo config set|get|list|rm` edits the store that
// config.Get reads. On this computer: $GOGO_CONFIG, else ~/.gogo/config,
// one key=value per line (the first line of a key wins). On the board:
// its NVS store, over the serial console (boardConfig; `scan` lists the
// Wi-Fi networks the board sees, `stats` its net stack's counters). See configMain for which one is used.
//
// Keys are 1-15 characters of [A-Za-z0-9._-] (the ESP32's NVS limit, so
// a configuration means the same on every platform); values are at most
// configMaxValue bytes without newlines. `get` and `list` never print the
// Wi-Fi password, only whether it is set.

const configUsage = "usage: gogo config [-port P | -local] set <key> <value|-> | get <key> | list | rm <key> | scan | stats"

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
	port, local := "", false
	for len(args) > 0 && strings.HasPrefix(args[0], "-") && args[0] != "-" {
		switch {
		case args[0] == "-local":
			local = true
			args = args[1:]
		case args[0] == "-port" && len(args) > 1:
			port = args[1]
			args = args[2:]
		default:
			return errors.New(configUsage)
		}
	}
	if len(args) == 0 || (port != "" && local) {
		return errors.New(configUsage)
	}
	op, args := args[0], args[1:]
	want := map[string]int{"set": 2, "get": 1, "list": 0, "rm": 1, "scan": 0, "stats": 0}
	if n, ok := want[op]; !ok || len(args) != n {
		return errors.New(configUsage)
	}
	if len(args) > 0 && !validKey(args[0]) {
		return fmt.Errorf("gogo config: bad key %q (1-%d characters of A-Z a-z 0-9 . _ -)", args[0], configMaxKey)
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
		if err := checkValue(args[1]); err != nil {
			return err
		}
	}
	if port == "" && !local && os.Getenv("GOGO_CONFIG") == "" {
		if ports, err := glob(portGlob); err == nil && len(ports) == 1 {
			port = ports[0]
			fmt.Fprintf(errOut, "gogo config: board on %s (-local for this computer's store)\n", port)
		}
	}
	if port != "" {
		f, err := openSerial(port)
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

// boardConfig sends one command frame to the board's console handler
// (platform/esp32s3/main/config_esp.c):
//
//	\x02gogo-config <op> [<key> [<value>]]\n
//
// and reads its reply: `\x02kv <text>` lines, then `\x02ok[ <text>]` or
// `\x02err <reason>`. Everything else on the console (program output) is
// skipped. The board never sends the Wi-Fi password back.
func boardConfig(rw io.ReadWriter, op string, args []string, out io.Writer, timeout time.Duration) error {
	frame := "\x02gogo-config " + op
	for _, a := range args {
		frame += " " + a
	}
	if _, err := io.WriteString(rw, frame+"\n"); err != nil {
		return err
	}
	lines := make(chan string)
	done := make(chan error, 1)
	quit := make(chan struct{})
	defer close(quit)
	go func() {
		var buf []byte
		in := false
		b := make([]byte, 512)
		for {
			n, err := rw.Read(b)
			for _, c := range b[:n] {
				switch {
				case c == 0x02:
					in, buf = true, buf[:0]
				case !in:
				case c == '\n':
					in = false
					select {
					case lines <- string(bytes.TrimSuffix(buf, []byte("\r"))):
					case <-quit:
						return
					}
				default:
					buf = append(buf, c)
				}
			}
			if err != nil {
				done <- err
				return
			}
		}
	}()
	timer := time.NewTimer(timeout)
	defer timer.Stop()
	var kvs []string
	for {
		select {
		case l := <-lines:
			tag, text, _ := strings.Cut(l, " ")
			switch tag {
			case "kv":
				kvs = append(kvs, text)
			case "ok":
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
			case "err":
				if text == "not set" && len(args) > 0 {
					return fmt.Errorf("gogo config: %s is not set", args[0])
				}
				return fmt.Errorf("gogo config: board: %s", text)
			}
		case err := <-done:
			if err == nil || err == io.EOF {
				err = errors.New("connection closed")
			}
			return fmt.Errorf("gogo config: board: %v", err)
		case <-timer.C:
			return errors.New("gogo config: no answer from the board (is gogo firmware running? another program using the port?)")
		}
	}
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
