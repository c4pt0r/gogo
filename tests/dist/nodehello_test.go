package disttest

import (
	"fmt"
	"strconv"
	"testing"
)

// examples/nodehello: the client's five Calls add up, and it sees the
// server go (NoConnection) when the server is killed.
func TestNodeHello(t *testing.T) {
	const ex = "../../examples/nodehello"
	port := freePort(t)
	srv := startArgs(t, ex, nil, []string{"server", strconv.Itoa(port)})
	srv.waitLine(fmt.Sprintf("server: listening on port %d", port))
	c := startArgs(t, ex, nil, []string{"client", fmt.Sprintf("127.0.0.1:%d", port)})
	for i, total := range []int{1, 3, 6, 10, 15} {
		c.waitLine(fmt.Sprintf("added %d total %d", i+1, total))
	}
	c.waitLine("client: done")

	c2 := startArgs(t, ex, nil, []string{"client", fmt.Sprintf("127.0.0.1:%d", port)})
	c2.waitLine("added 2 total 18")
	srv.kill()
	c2.waitLine("server gone, reason 11")
}
