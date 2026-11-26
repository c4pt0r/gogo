// Package disttest runs distributed-actor scenarios (gogo/node): a server
// node and boards as separate VMs ($GOGO_VM --host-net) talking real TCP
// on 127.0.0.1. Each program prints what it saw; the test compares.
package disttest

import (
	"bufio"
	"fmt"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"syscall"
	"testing"
	"time"

	"gogotools/compiler"
)

const token = "a long shared test token"

// node is one VM process.
type node struct {
	t     *testing.T
	name  string
	cmd   *exec.Cmd
	mu    sync.Mutex
	lines []string
	more  chan struct{}
	done  chan struct{}
	err   error
	errs  string // stderr (crash reports), logged on failure
}

func build(t *testing.T, prog string) string {
	t.Helper()
	dir := filepath.Join("progs", prog)
	if strings.Contains(prog, "/") { // an example: ../../examples/<name>
		dir, prog = prog, filepath.Base(prog)
	}
	p, err := compiler.Load(dir)
	if err != nil {
		t.Fatalf("load %s: %v", prog, err)
	}
	img, err := compiler.Compile(p)
	if err != nil {
		t.Fatalf("compile %s: %v", prog, err)
	}
	out := filepath.Join(t.TempDir(), prog+".ggb")
	if err := os.WriteFile(out, img, 0o644); err != nil {
		t.Fatal(err)
	}
	return out
}

// start runs prog with the config lines cfg (node.token is added unless
// cfg sets it).
func start(t *testing.T, prog string, cfg ...string) *node {
	return startVM(t, prog, nil, cfg...)
}

// startVM is start with extra VM flags.
func startVM(t *testing.T, prog string, flags []string, cfg ...string) *node {
	t.Helper()
	return startArgs(t, prog, flags, nil, cfg...)
}

// startArgs is startVM with the program's arguments (sys.Args).
func startArgs(t *testing.T, prog string, flags, args []string, cfg ...string) *node {
	t.Helper()
	return startOn(t, "GOGO_VM", prog, flags, args, cfg...)
}

// startOn is startArgs on the VM named by the environment variable env
// (GOGO_VM, or GOGO_VM_MCU: the board configuration; skipped if unset).
func startOn(t *testing.T, env, prog string, flags, args []string, cfg ...string) *node {
	t.Helper()
	vm := os.Getenv(env)
	if vm == "" && env != "GOGO_VM" {
		t.Skipf("%s is not set", env)
	}
	if vm == "" {
		t.Fatal("GOGO_VM is not set: run `make test-go`, or build the VM with `make vm` and set GOGO_VM=$PWD/build/gogovm")
	}
	conf := filepath.Join(t.TempDir(), "config")
	if err := os.WriteFile(conf, []byte(strings.Join(append(cfg, "node.token="+token), "\n")+"\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	n := &node{t: t, name: filepath.Base(prog), more: make(chan struct{}, 1), done: make(chan struct{})}
	argv := append(append([]string{"--host-net"}, flags...), build(t, prog))
	if len(flags) > 0 && flags[0] == "--no-host-net" { // the simulated stack, with no network
		argv = append(append([]string{}, flags[1:]...), build(t, prog))
	}
	n.cmd = exec.Command(vm, append(argv, args...)...)
	n.cmd.Env = append(os.Environ(), "GOGO_CONFIG="+conf)
	out, err := n.cmd.StdoutPipe()
	if err != nil {
		t.Fatal(err)
	}
	var stderr strings.Builder
	n.cmd.Stderr = &stderr
	if err := n.cmd.Start(); err != nil {
		t.Fatal(err)
	}
	go func() {
		sc := bufio.NewScanner(out)
		for sc.Scan() {
			n.mu.Lock()
			n.lines = append(n.lines, sc.Text())
			n.mu.Unlock()
			select {
			case n.more <- struct{}{}:
			default:
			}
		}
		n.err = n.cmd.Wait()
		n.errs = stderr.String()
		close(n.done)
	}()
	t.Cleanup(func() {
		n.cmd.Process.Kill()
		<-n.done
		if t.Failed() {
			t.Logf("--- %s ---\n%s\n--- %s stderr ---\n%s", n.name, strings.Join(n.output(), "\n"), n.name, n.errs)
		}
	})
	return n
}

func (n *node) output() []string {
	n.mu.Lock()
	defer n.mu.Unlock()
	return append([]string(nil), n.lines...)
}

// waitLine waits until the node printed line.
func (n *node) waitLine(line string) {
	n.t.Helper()
	deadline := time.After(15 * time.Second)
	for {
		for _, l := range n.output() {
			if l == line {
				return
			}
		}
		select {
		case <-n.more:
		case <-n.done:
			for _, l := range n.output() {
				if l == line {
					return
				}
			}
			n.t.Fatalf("%s exited (%v) without printing %q", n.name, n.err, line)
		case <-deadline:
			n.t.Fatalf("%s: no %q within 15 s", n.name, line)
		}
	}
}

// finish waits for the node to exit by itself and compares its output.
func (n *node) finish(want string) {
	n.t.Helper()
	select {
	case <-n.done:
	case <-time.After(20 * time.Second):
		n.t.Fatalf("%s still running", n.name)
	}
	if got := strings.Join(n.output(), "\n") + "\n"; got != want {
		n.t.Errorf("%s output\n--- got ---\n%s--- want ---\n%s", n.name, got, want)
	}
}

// wait waits for the node to exit by itself.
func (n *node) wait() {
	n.t.Helper()
	select {
	case <-n.done:
	case <-time.After(20 * time.Second):
		n.t.Fatalf("%s still running", n.name)
	}
}

func (n *node) kill() {
	n.cmd.Process.Signal(syscall.SIGKILL)
	<-n.done
}

func freePort(t *testing.T) int {
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer l.Close()
	return l.Addr().(*net.TCPAddr).Port
}

// A server and three boards: remote Send and Call, own types by name, a
// slice, Monitor and Link with the remote exit code, a value that may not
// leave its node, the relay between boards, a refused token, and a lost
// board.
func TestStar(t *testing.T) {
	port := freePort(t)
	addr := fmt.Sprintf("test.server=127.0.0.1:%d", port)
	srv := start(t, "server", fmt.Sprintf("test.port=%d", port))
	srv.waitLine("listening")
	b := start(t, "board_b", addr)
	b.waitLine("b ready")
	c := start(t, "board_c", addr)
	c.finish("connect refused true\nwhereis closed true\nself true\n")
	a := start(t, "board_a", addr)
	a.waitLine("a waiting")
	b.kill()
	a.finish(`connect true a
nodes 2
whereis echo true true
whereis nobody true
whereis nonode true
register twice true true
call true hi
call slice true
pong 42 server
same pid true
down true true 123
exit true 124
killed true 125
link killed true true
sendafter true true
shape mismatch answered false
badmsg true true
whereis becho true
relay call true hello from b
a waiting
b down true true
node down b
nodes 1
done
`)
	srv.waitLine("down a")
	for _, l := range srv.output() {
		if l == "up c" {
			t.Error("the server let board c in with a wrong token")
		}
	}
}

// A board loses the server: its monitor and link of a server actor fire
// with NoConnection, and node.Down names the server; it can connect again.
func TestServerLost(t *testing.T) {
	port := freePort(t)
	srv := start(t, "server", fmt.Sprintf("test.port=%d", port))
	srv.waitLine("listening")
	w := start(t, "watcher", fmt.Sprintf("test.server=127.0.0.1:%d", port))
	w.waitLine("w ready")
	srv.kill()
	w.wait()
	got := w.output()
	if len(got) != 7 {
		t.Fatalf("watcher output %q", got)
	}
	events := append([]string(nil), got[1:5]...)
	sort.Strings(events)
	want := []string{"call in flight false true", "down true 11", "exit 11", "node down server"}
	if strings.Join(events, "|") != strings.Join(want, "|") || got[0] != "w ready" ||
		got[5] != "call to lost node false true" || got[6] != "connect again false" {
		t.Errorf("watcher output %q", got)
	}
}

// Without a network (gogovm without --host-net or --net-fd), or without
// the drivers, Listen and Connect say Unsupported.
func TestNoNetwork(t *testing.T) {
	for _, flags := range [][]string{{"--no-host-net"}, {"--without", "tcp,tcp_listen,node"}} {
		n := startVM(t, "nonet", flags)
		n.finish("listen true\nconnect true\n")
	}
}

// Killing a board's node agent ends the node: remote monitors fire with
// NoConnection, the server sees the board go, and Connect starts a new
// node that works.
func TestNodeRestartable(t *testing.T) {
	srv, port := startServer(t)
	b := start(t, "restart", fmt.Sprintf("test.server=127.0.0.1:%d", port))
	for _, l := range []string{"connect true", "whereis true", "down true true true", "self after true",
		"connect again true", "whereis again true", "call true hi"} {
		b.waitLine(l)
	}
	srv.waitLine("down phoenix")
	noCrash(t, srv)
}

// A server whose agent dies can Listen again, and boards join it.
func TestServerRestartable(t *testing.T) {
	port := freePort(t)
	srv := start(t, "relisten", fmt.Sprintf("test.port=%d", port))
	srv.waitLine("listen true")
	srv.waitLine("listen again true")
	srv.waitLine("listening")
	p, err := dial(t, port, "after", token)
	if err != nil {
		t.Fatal(err)
	}
	if p.whereis("echo") == 0 {
		t.Error("echo not found")
	}
}
