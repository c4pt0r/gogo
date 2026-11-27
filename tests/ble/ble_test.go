// Package bletest runs programs that open the "ble" port on gogovm
// ($GOGO_VM), with the test as the phone: gogovm's ble port is a Unix
// socket ($GOGO_BLE, platform/unix/ble_unix.c, which documents the line
// protocol), and a phone here connects, reads, writes and subscribes over
// it as a central would over the air.
//
// The book's BLE examples (docs/book/examples/*/phone) run here too: a
// phone file is a script of lines, "> line" sent, "< line" expected next,
// "eof" the end of the connection, "# ..." a comment; the program's
// stdout must then equal expected.out.
package bletest

import (
	"bufio"
	"bytes"
	"fmt"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"gogotools/compiler"
)

const timeout = 10 * time.Second

func vmPath(t *testing.T) string {
	t.Helper()
	vm := os.Getenv("GOGO_VM")
	if vm == "" {
		t.Skip("GOGO_VM not set (make test-go sets it)")
	}
	return vm
}

func build(t *testing.T, dir string) string {
	t.Helper()
	p, err := compiler.Load(dir)
	if err != nil {
		t.Fatalf("load %s: %v", dir, err)
	}
	img, err := compiler.Compile(p)
	if err != nil {
		t.Fatalf("compile %s: %v", dir, err)
	}
	out := filepath.Join(t.TempDir(), "prog.ggb")
	if err := os.WriteFile(out, img, 0o644); err != nil {
		t.Fatal(err)
	}
	return out
}

// vm is one gogovm running a program, its ble socket at sock.
type vm struct {
	t      *testing.T
	cmd    *exec.Cmd
	sock   string
	stdout *lines
	stderr bytes.Buffer
	done   chan error
}

// lines collects a stream's lines for waitLine.
type lines struct {
	ch  chan string
	all []string
}

func newLines() *lines { return &lines{ch: make(chan string, 4096)} }

func (l *lines) feed(r *bufio.Scanner) {
	for r.Scan() {
		l.ch <- r.Text()
	}
	close(l.ch)
}

// wait returns the next line, or fails the test after the timeout.
func (l *lines) wait(t *testing.T) string {
	t.Helper()
	select {
	case s, ok := <-l.ch:
		if !ok {
			t.Fatalf("program output ended; got %q", l.all)
		}
		l.all = append(l.all, s)
		return s
	case <-time.After(timeout):
		t.Fatalf("no program output for %v; got %q", timeout, l.all)
	}
	return ""
}

// rest returns every line until the program's output ends.
func (l *lines) rest(t *testing.T) []string {
	t.Helper()
	for {
		select {
		case s, ok := <-l.ch:
			if !ok {
				return l.all
			}
			l.all = append(l.all, s)
		case <-time.After(timeout):
			t.Fatalf("program output did not end; got %q", l.all)
		}
	}
}

// start runs img on gogovm with its ble socket in a fresh directory (a
// short path: a socket's path has a limit of about 100 bytes).
func start(t *testing.T, img string, flags ...string) *vm {
	t.Helper()
	dir, err := os.MkdirTemp("", "gogoble")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { os.RemoveAll(dir) })
	v := &vm{t: t, sock: filepath.Join(dir, "b.sock"), stdout: newLines(), done: make(chan error, 1)}
	v.cmd = exec.Command(vmPath(t), append(flags, img)...)
	v.cmd.Env = append(os.Environ(), "GOGO_BLE="+v.sock, "GOGO_CONFIG="+filepath.Join(dir, "no-config"))
	v.cmd.Stderr = &v.stderr
	out, err := v.cmd.StdoutPipe()
	if err != nil {
		t.Fatal(err)
	}
	if err := v.cmd.Start(); err != nil {
		t.Fatal(err)
	}
	go v.stdout.feed(bufio.NewScanner(out))
	go func() { v.done <- v.cmd.Wait() }()
	t.Cleanup(func() {
		v.cmd.Process.Kill()
		<-v.done
		if t.Failed() && v.stderr.Len() > 0 {
			t.Logf("gogovm stderr:\n%s", v.stderr.String())
		}
	})
	return v
}

// exit waits for the program to end and returns its stdout and status.
func (v *vm) exit() ([]string, error) {
	v.t.Helper()
	out := v.stdout.rest(v.t)
	select {
	case err := <-v.done:
		v.done <- err
		return out, err
	case <-time.After(timeout):
		v.t.Fatalf("gogovm did not exit")
	}
	return nil, nil
}

// phone is a central on the VM's socket.
type phone struct {
	t *testing.T
	c net.Conn
	r *bufio.Reader
}

// dial connects to the VM's socket once the port has opened it.
func (v *vm) dial() *phone {
	v.t.Helper()
	deadline := time.Now().Add(timeout)
	for {
		c, err := net.Dial("unix", v.sock)
		if err == nil {
			return &phone{t: v.t, c: c, r: bufio.NewReader(c)}
		}
		if time.Now().After(deadline) {
			v.t.Fatalf("ble socket %s: %v", v.sock, err)
		}
		time.Sleep(10 * time.Millisecond)
	}
}

func (p *phone) send(line string) {
	p.t.Helper()
	if _, err := fmt.Fprintf(p.c, "%s\n", line); err != nil {
		p.t.Fatalf("send %q: %v", line, err)
	}
}

// recv returns the next line from the VM ("" and false at the end). A
// timeout fails the test: it is not the end of the connection.
func (p *phone) recv() (string, bool) {
	p.t.Helper()
	p.c.SetReadDeadline(time.Now().Add(timeout))
	s, err := p.r.ReadString('\n')
	if err != nil {
		if ne, ok := err.(net.Error); ok && ne.Timeout() {
			p.t.Fatalf("phone: nothing from the VM for %v (got %q)", timeout, s)
		}
		if s == "" {
			return "", false
		}
	}
	return strings.TrimSuffix(s, "\n"), true
}

func (p *phone) expect(want string) {
	p.t.Helper()
	got, ok := p.recv()
	if !ok || got != want {
		p.t.Fatalf("phone: got %q (open %v), want %q", got, ok, want)
	}
}

func (p *phone) expectEOF() {
	p.t.Helper()
	if got, ok := p.recv(); ok {
		p.t.Fatalf("phone: got %q, want the end of the connection", got)
	}
}

// gatt reads the advertisement and services the VM sends on accept.
func (p *phone) gatt() []string {
	p.t.Helper()
	var g []string
	for {
		s, ok := p.recv()
		if !ok {
			p.t.Fatalf("phone: connection ended in the GATT table: %q", g)
		}
		if s == "end" {
			return g
		}
		g = append(g, s)
	}
}

// script plays a phone file (see the package comment).
func (p *phone) script(name string, text string) {
	p.t.Helper()
	for i, l := range strings.Split(text, "\n") {
		switch {
		case l == "" || strings.HasPrefix(l, "#"):
		case strings.HasPrefix(l, "> "):
			p.send(l[2:])
		case strings.HasPrefix(l, "< "):
			got, ok := p.recv()
			if !ok || got != l[2:] {
				p.t.Fatalf("%s:%d: got %q (open %v), want %q", name, i+1, got, ok, l[2:])
			}
		case l == "eof":
			if got, ok := p.recv(); ok {
				p.t.Fatalf("%s:%d: got %q, want the end of the connection", name, i+1, got)
			}
		default:
			p.t.Fatalf("%s:%d: bad line %q", name, i+1, l)
		}
	}
}

func hexs(s string) string { return fmt.Sprintf("%x", s) }

const echo = "progs/echo"

var echoGATT = []string{
	"adv gogo-echo",
	"svc 6e400001-b5a3-f393-e0a9-e50e24dcca9e",
	"chr 0 6e400004-b5a3-f393-e0a9-e50e24dcca9e 9",
	"chr 1 6e400002-b5a3-f393-e0a9-e50e24dcca9e 6",
	"chr 2 6e400003-b5a3-f393-e0a9-e50e24dcca9e 8",
}

// The whole round: GATT table, connect, read, subscribe, write with and
// without response, notifications back, the counter's value and its
// notification, the ATT errors of what a characteristic does not allow.
func TestEcho(t *testing.T) {
	v := start(t, build(t, echo))
	if s := v.stdout.wait(t); s != "open" {
		t.Fatalf("got %q", s)
	}
	p := v.dial()
	if g := p.gatt(); strings.Join(g, "\n") != strings.Join(echoGATT, "\n") {
		t.Fatalf("GATT:\n%s", strings.Join(g, "\n"))
	}
	p.send("connect 11:22:33:44:55:66 23")
	if s := v.stdout.wait(t); s != "connected 23" {
		t.Fatalf("got %q", s)
	}
	p.send("read 0")
	p.expect("value 0 " + hexs("0"))
	p.send("read 2") // Notify only
	p.expect("error 2 2")
	p.send("write 0 00") // Read | Notify only
	p.expect("error 0 3")
	p.send("sub 1 1") // no Notify
	p.expect("error 1 6")
	p.send("sub 2 1")
	p.expect("ok 2")
	p.send("sub 0 1")
	p.expect("ok 0")
	p.send("write 1 " + hexs("hello"))
	p.expect("ok 1")
	p.expect("notify 0 " + hexs("1"))
	p.expect("notify 2 " + hexs("hello"))
	p.send("writenr 1 " + hexs("again"))
	p.expect("notify 0 " + hexs("2"))
	p.expect("notify 2 " + hexs("again"))
	p.send("read 0")
	p.expect("value 0 " + hexs("2"))
	p.send("sub 0 0")
	p.expect("ok 0")
	p.send("big") // not a command of the protocol: ignored
	// "big" (refused: Done -1) and "bye" in one segment: "bye" is in the
	// program's mailbox while it waits for the Done of "big", and must not
	// be lost there
	p.send("write 1 " + hexs("big") + "\nwrite 1 " + hexs("bye"))
	p.expect("ok 1")
	p.expect("ok 1")
	p.expectEOF()
	out, err := v.exit()
	want := []string{"open", "connected 23", "subscribed 2 true", "subscribed 0 true", "echo hello", "echo again",
		"subscribed 0 false", "big: -1", "bye"}
	if err != nil || strings.Join(out, "\n") != strings.Join(want, "\n") {
		t.Fatalf("exit %v, output:\n%s", err, strings.Join(out, "\n"))
	}
}

// One phone at a time: a second is told busy; after the first leaves
// (the link drops), the port advertises again and a new phone connects.
func TestOnePhoneAtATime(t *testing.T) {
	v := start(t, build(t, echo))
	v.stdout.wait(t)
	a := v.dial()
	a.gatt()
	a.send("connect aa:aa:aa:aa:aa:aa 23")
	if s := v.stdout.wait(t); s != "connected 23" {
		t.Fatalf("got %q", s)
	}
	b := v.dial()
	b.expect("busy")
	b.expectEOF()
	a.send("drop")
	a.expectEOF()
	if s := v.stdout.wait(t); s != "disconnected 8" {
		t.Fatalf("got %q", s)
	}
	c := v.dial()
	c.gatt()
	c.send("connect cc:cc:cc:cc:cc:cc 247")
	if s := v.stdout.wait(t); s != "connected 247" {
		t.Fatalf("got %q", s)
	}
	c.c.Close() // the phone goes away: reason 0x13
	if s := v.stdout.wait(t); s != "disconnected 19" {
		t.Fatalf("got %q", s)
	}
}

// Notifications with flow control: the program keeps two in flight and
// waits for each Done; every one arrives, in order, MTU - 3 bytes each.
func TestFlood(t *testing.T) {
	v := start(t, build(t, echo))
	v.stdout.wait(t)
	p := v.dial()
	p.gatt()
	p.send("connect 11:22:33:44:55:66 23")
	p.send("mtu 185")
	p.send("sub 2 1")
	p.expect("ok 2")
	for _, w := range []string{"connected 23", "mtu 185", "subscribed 2 true"} {
		if s := v.stdout.wait(t); s != w {
			t.Fatalf("got %q, want %q", s, w)
		}
	}
	const n = 500
	p.send("write 1 " + hexs(fmt.Sprintf("flood %d", n)))
	p.expect("ok 1")
	chunk := make([]byte, 182)
	for i := range chunk {
		chunk[i] = byte('a' + i%26)
	}
	want := "notify 2 " + hexs(string(chunk))
	t0 := time.Now()
	for i := 0; i < n; i++ {
		got, ok := p.recv()
		if !ok || got != want {
			t.Fatalf("notification %d: %q", i, got)
		}
	}
	t.Logf("%d notifications of 182 bytes in %v", n, time.Since(t0))
	if s := v.stdout.wait(t); !strings.HasPrefix(s, fmt.Sprintf("flood %d x 182 ok %d failed 0 ", n, n)) {
		t.Fatalf("got %q", s)
	}
	// one byte over MTU - 3: refused
	p.send("write 1 " + hexs("big"))
	p.expect("ok 1")
	if s := v.stdout.wait(t); s != "big: -1" {
		t.Fatalf("got %q", s)
	}
}

// Config errors, a second open, and an image refused by a VM without ble.
func TestOpenErrors(t *testing.T) {
	img := build(t, "progs/errors")
	v := start(t, img)
	out, err := v.exit()
	want, rerr := os.ReadFile("progs/errors/expected.out")
	if rerr != nil {
		t.Fatal(rerr)
	}
	if err != nil || strings.Join(out, "\n")+"\n" != string(want) {
		t.Fatalf("exit %v, output:\n%s", err, strings.Join(out, "\n"))
	}
	cmd := exec.Command(vmPath(t), "--without", "ble", img)
	b, err := cmd.CombinedOutput()
	if ee, ok := err.(*exec.ExitError); !ok || ee.ExitCode() != 3 || !strings.Contains(string(b), "requires ble") {
		t.Fatalf("--without ble: %v\n%s", err, b)
	}
}

// The book's examples with a phone script.
func TestBookExamples(t *testing.T) {
	dirs, err := filepath.Glob("../../docs/book/examples/*/phone")
	if err != nil {
		t.Fatal(err)
	}
	if len(dirs) == 0 {
		t.Fatal("no book example has a phone script")
	}
	for _, ph := range dirs {
		d := filepath.Dir(ph)
		t.Run(filepath.Base(d), func(t *testing.T) {
			script, err := os.ReadFile(ph)
			if err != nil {
				t.Fatal(err)
			}
			want, err := os.ReadFile(filepath.Join(d, "expected.out"))
			if err != nil {
				t.Fatal(err)
			}
			v := start(t, build(t, d))
			v.stdout.wait(t) // the program's first line: it has set its values
			p := v.dial()
			p.script(ph, string(script))
			out, err := v.exit()
			if err != nil || strings.Join(out, "\n")+"\n" != string(want) {
				t.Fatalf("exit %v, output:\n%s\nwant:\n%s", err, strings.Join(out, "\n"), want)
			}
		})
	}
}
