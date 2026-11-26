package disttest

import (
	"fmt"
	"strconv"
	"testing"
)

// A board in a star of n other nodes still finds the server's counter
// and calls it: door proxies are made on demand, not one per node, so the
// board's 64 remote PIDs (8 links) are enough: 70 nodes, on a host VM
// with the board's caps.
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

func TestBoardInStarOf70(t *testing.T) { starOf(t, 70, "GOGO_VM") }
