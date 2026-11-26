package disttest

import (
	"fmt"
	"os"
	"strconv"
	"strings"
	"testing"
)

// fill joins n Go peers to a server started with flags (none: the host
// defaults, 1024 ports and 512 links), checks that some can still look up
// echo and that the server's node agent survived, then that one more
// board is refused (the link cap) while the others stay.
func fill(t *testing.T, n int, flags ...string) {
	port := freePort(t)
	srv := startVM(t, "server", flags, fmt.Sprintf("test.port=%d", port))
	srv.waitLine("listening")
	var ps []*peer
	for i := 0; i < n; i++ {
		p, err := dial(t, port, fmt.Sprintf("q%d", i), token)
		if err != nil {
			t.Fatalf("board %d of %d: %v", i+1, n, err)
		}
		ps = append(ps, p)
	}
	for _, i := range []int{0, n / 2, n - 1} {
		if ps[i].whereis("echo") == 0 {
			t.Errorf("board %d: echo not found", i)
		}
	}
	if _, err := dial(t, port, "one-more", token); err == nil {
		t.Errorf("board %d joined past the link cap", n+1)
	}
	if ps[0].whereis("echo") == 0 {
		t.Error("after the refusal: echo not found")
	}
	srv.kill()
	if strings.Contains(srv.errs, "crashed") {
		t.Errorf("server stderr: %s", srv.errs)
	}
	for _, p := range ps {
		p.c.Close()
	}
}

// The host defaults: 512 boards (the documented number).
func TestServerFill(t *testing.T) { fill(t, 512) }

// With the caps at the ceiling (2048 ports, 2047 links), boards join
// until the server's node agent is near its memory limit (256 KB): from
// then on new boards are refused, and the server and its boards carry on.
// Run with GOGO_DIST_SCALE set (make test-dist-scale): about 2000 file
// descriptors in the test and as many in the VM.
func TestServerFillCeiling(t *testing.T) {
	if os.Getenv("GOGO_DIST_SCALE") == "" {
		t.Skip("GOGO_DIST_SCALE not set (make test-dist-scale)")
	}
	port := freePort(t)
	srv := startVM(t, "server", []string{"--max-ports", "2048", "--max-links", "2047"}, fmt.Sprintf("test.port=%d", port))
	srv.waitLine("listening")
	var ps []*peer
	for i := 0; i < 2047; i++ {
		p, err := dial(t, port, fmt.Sprintf("q%d", i), token)
		if err != nil {
			break
		}
		ps = append(ps, p)
	}
	t.Logf("%d boards joined", len(ps))
	if len(ps) < 900 {
		t.Errorf("only %d boards joined", len(ps))
	}
	for _, i := range []int{0, len(ps) - 1} {
		if ps[i].whereis("echo") == 0 {
			t.Errorf("board %d: echo not found", i)
		}
	}
	ps[0].c.Close() // room again for one
	srv.waitLine("down q0")
	if p, err := dial(t, port, "again", token); err != nil {
		t.Errorf("no room after a board left: %v", err)
	} else {
		ps[0] = p
	}
	srv.kill()
	if strings.Contains(srv.errs, "crashed") {
		t.Errorf("server stderr: %s", srv.errs)
	}
	for _, p := range ps {
		p.c.Close()
	}
}

// A board in a star of n other nodes still finds the server's counter
// and calls it: door proxies are made on demand, not one per node, so the
// board's 64 remote PIDs (8 links) are enough. 70 on a host VM with the
// board's caps; 300 on the MCU VM (the board's arena and limits).
func starOf(t *testing.T, n int, env string) {
	const ex = "../../examples/nodehello"
	port := freePort(t)
	srv := startArgs(t, ex, nil, []string{"server", strconv.Itoa(port)})
	srv.waitLine(fmt.Sprintf("server: listening on port %d", port))
	var ps []*peer // kept: a collected peer closes its connection
	for i := 0; i < n; i++ {
		p, err := dial(t, port, fmt.Sprintf("p%d", i), token)
		if err != nil {
			t.Fatalf("p%d: %v", i, err)
		}
		ps = append(ps, p)
	}
	defer func() {
		for _, p := range ps {
			p.c.Close()
		}
	}()
	c := startOn(t, env, ex, []string{"--max-links", "8", "--max-ports", "16"}, []string{"client", fmt.Sprintf("127.0.0.1:%d", port)})
	c.waitLine("added 1 total 1")
	c.waitLine("added 2 total 3")
}

func TestBoardInStarOf70(t *testing.T)     { starOf(t, 70, "GOGO_VM") }
func TestMCUBoardInStarOf300(t *testing.T) { starOf(t, 300, "GOGO_VM_MCU") }
