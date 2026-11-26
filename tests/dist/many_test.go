package disttest

import (
	"fmt"
	"testing"
)

// A server with 24 ports serves 20 boards (each link costs it one port:
// the node port takes the TCP connection over), and every board Calls the
// next one's echo through the relay.
func TestManyBoards(t *testing.T) {
	const boards = 20
	port := freePort(t)
	srv := startVM(t, "server", []string{"--max-ports", "24"}, fmt.Sprintf("test.port=%d", port))
	srv.waitLine("listening")
	var bs []*node
	for i := 0; i < boards; i++ {
		b := startVM(t, "board_n", []string{"--max-ports", "4"},
			fmt.Sprintf("test.server=127.0.0.1:%d", port), fmt.Sprintf("test.name=b%d", i),
			fmt.Sprintf("test.next=b%d", (i+1)%boards), fmt.Sprintf("test.count=%d", boards))
		bs = append(bs, b)
	}
	for i, b := range bs {
		b.waitLine(fmt.Sprintf("b%d true hi from b%d", i, (i+1)%boards))
	}
	for i := 0; i < boards; i++ {
		srv.waitLine(fmt.Sprintf("up b%d", i))
	}
}
