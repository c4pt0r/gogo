package nettest

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"gogotools/compiler"
	"gogotools/tests/net/sim"
)

// startBook builds the book example docs/book/examples/<name> and starts
// it like startHTTP: a static address (10.0.0.2/24, gateway 10.0.0.1) and
// the gateway's ARP answered.
func startBook(t *testing.T, name string) *sim.VM {
	t.Helper()
	prog, err := compiler.Load(filepath.Join("..", "..", "docs", "book", "examples", name))
	if err != nil {
		t.Fatalf("load %s: %v", name, err)
	}
	img, err := compiler.Compile(prog)
	if err != nil {
		t.Fatalf("compile %s: %v", name, err)
	}
	dir := t.TempDir()
	image := filepath.Join(dir, name+".ggb")
	if err := os.WriteFile(image, img, 0o644); err != nil {
		t.Fatal(err)
	}
	cfg := filepath.Join(dir, "config")
	if err := os.WriteFile(cfg, []byte("net.ip=10.0.0.2\nnet.gw=10.0.0.1\nnet.dns=10.0.0.1\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	t.Setenv("GOGO_CONFIG", cfg)
	vmPath := os.Getenv("GOGO_VM")
	if vmPath == "" {
		t.Fatal("GOGO_VM is not set: run `make test-go`, or build the VM with `make vm` and set GOGO_VM=$PWD/build/gogovm")
	}
	v, err := sim.Start(vmPath, image, realLimit)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		v.Wait()
		if t.Failed() {
			t.Logf("--- stdout ---\n%s--- stderr ---\n%s", v.Stdout.Bytes(), v.Stderr.Bytes())
		}
	})
	arpFor(t, one(t)(v.Sync()), vmHost.IP, gwHost.IP)
	none(t)(v.Exchange(arpReply(gwHost, vmHost)))
	return v
}

// lineConn connects to the line server on port 7000: the listener hands
// the paused connection to a handler, which opens the window.
func lineConn(t *testing.T, v *sim.VM, pport uint16) *tcpConn {
	t.Helper()
	c := newConn(t, v, pport, 7000)
	s := c.send(sim.SYN, nil, sim.MSSOption(1460)...)
	if len(s) != 1 || s[0].Flags != sim.SYN|sim.ACK || s[0].Window != 0 {
		t.Fatalf("want SYN-ACK with window 0, got %s", segsStr(s))
	}
	c.ack = s[0].Seq + 1
	if x := c.expect(c.send(sim.ACK, nil), sim.ACK, ""); x.Window != 1460 {
		t.Fatalf("window %d after handoff", x.Window)
	}
	return c
}

// say sends one segment and acknowledges what the server sends back until
// it stops; it returns that data and whether the server's FIN came.
func (c *tcpConn) say(flags uint8, payload string) (string, bool) {
	c.t.Helper()
	got, fin := c.data(c.mine(c.send(flags, []byte(payload))))
	for i := 0; !fin; i++ {
		more, f := c.data(c.mine(c.send(sim.ACK, nil)))
		if more == "" && !f {
			break
		}
		if i == 20 {
			c.t.Fatalf("server keeps sending; so far %q", got)
		}
		got, fin = got+more, f
	}
	return got, fin
}

// Chapter 21's line server (docs/book/examples/21-line-server) on the
// simulated network: each connection gets its own handler; lines split
// anywhere over segments are joined and answered only when complete; two
// connections are served at once; the peer's FIN gets the remaining
// answers, then the server's FIN (Shutdown); a line over MaxLine resets
// that connection only.
func TestBookLineServer(t *testing.T) {
	v := startBook(t, "21-line-server")
	a := lineConn(t, v, 41001)
	if got, _ := a.say(sim.PSH|sim.ACK, "pi"); got != "" {
		t.Fatalf("answer to half a line: %q", got)
	}
	b := lineConn(t, v, 41002) // a second client while the first is open
	if got, _ := a.say(sim.PSH|sim.ACK, "ng\nhello wor"); got != "pong\n" {
		t.Fatalf("a: got %q, want pong", got)
	}
	if got, _ := b.say(sim.PSH|sim.ACK, "ping\n"); got != "pong\n" {
		t.Fatalf("b: got %q, want pong", got)
	}
	if got, _ := a.say(sim.PSH|sim.ACK, "ld\nping\n"); got != "echo: hello world\npong\n" {
		t.Fatalf("a: got %q", got)
	}
	if got, fin := a.say(sim.PSH|sim.ACK, "bye"); got != "" || fin {
		t.Fatalf("a: answer to an unfinished line: %q fin %v", got, fin)
	}
	// The peer finishes: the unfinished "bye" gets no answer, the server
	// shuts down its side.
	if got, fin := a.say(sim.FIN|sim.ACK, ""); got != "" || !fin {
		t.Fatalf("a: after our FIN got %q fin %v, want the server's FIN", got, fin)
	}
	if s := a.mine(a.send(sim.ACK, nil)); len(s) != 0 { // our ACK of its FIN ends it
		t.Fatalf("a: after the close: %s", segsStr(s))
	}
	// b sends a line longer than MaxLine (128): reset; the server goes on.
	b.rst(b.mine(b.send(sim.PSH|sim.ACK, []byte(strings.Repeat("x", 200)))))
	c := lineConn(t, v, 41003)
	if got, _ := c.say(sim.PSH|sim.ACK, "ping\n"); got != "pong\n" {
		t.Fatalf("c: got %q, want pong", got)
	}
	stop(t, v, "serving lines on port 7000\n")
}
