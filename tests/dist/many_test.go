package disttest

import (
	"fmt"
	"strings"
	"testing"
	"time"
)

// manyBoards starts a server with srvFlags and n boards, each with 4
// ports, and checks that every board joins and Calls the next one's echo
// through the relay. With at, every board's Connect starts at the same
// moment (test.at, a wall clock time after all have started), so most of
// them find the server's handshake slots taken: they join by retrying
// with backoff (join in progs/board_n), as the book recommends.
func manyBoards(t *testing.T, n int, at bool, srvFlags ...string) {
	port := freePort(t)
	srv := startVM(t, "server", srvFlags, fmt.Sprintf("test.port=%d", port))
	srv.waitLine("listening")
	var extra []string
	if at {
		// Long enough for n VMs to start, even under a sanitizer on a slow
		// runner; a board that starts late says so, and joins at once.
		extra = append(extra, fmt.Sprintf("test.at=%d", time.Now().Add(3*time.Second+time.Duration(n)*150*time.Millisecond).UnixMilli()))
	}
	var bs []*node
	for i := 0; i < n; i++ {
		cfg := append([]string{fmt.Sprintf("test.server=127.0.0.1:%d", port), fmt.Sprintf("test.name=b%d", i),
			fmt.Sprintf("test.next=b%d", (i+1)%n), fmt.Sprintf("test.count=%d", n)}, extra...)
		bs = append(bs, startVM(t, "board_n", []string{"--max-ports", "4"}, cfg...))
	}
	for i, b := range bs {
		b.waitLine(fmt.Sprintf("b%d true hi from b%d", i, (i+1)%n))
	}
	for i := 0; i < n; i++ {
		srv.waitLine(fmt.Sprintf("up b%d", i))
	}
	late, busy, other := 0, 0, 0
	for i, b := range bs {
		for _, l := range b.output() {
			if l == fmt.Sprintf("b%d late", i) {
				late++
			}
			if l == fmt.Sprintf("b%d retry: busy", i) {
				busy++
			} else if strings.HasPrefix(l, fmt.Sprintf("b%d retry: ", i)) {
				other++
			}
		}
	}
	t.Logf("%d boards, %d late, %d retries after Busy, %d after other errors", n, late, busy, other)
	if at && late == 0 && busy == 0 && n > 2*slotsOf(srvFlags) {
		t.Errorf("%d boards at once, more than twice the slots, and no Connect saw Busy", n)
	}
	if strings.Contains(srv.errs, "crashed") {
		t.Errorf("server stderr: %s", srv.errs)
	}
}

// slotsOf is the handshake slots of a server started with flags (the link
// cap / 32, at least 2: lib/node's handshakeSlots).
func slotsOf(flags []string) int {
	links := 512
	for i, f := range flags {
		if f == "--max-links" {
			fmt.Sscan(flags[i+1], &links)
		}
	}
	return max(2, min(64, links/32))
}

// A server with 24 ports serves 20 boards (each link costs it one port:
// the node port takes the TCP connection over), and every board Calls the
// next one's echo through the relay.
func TestManyBoards(t *testing.T) { manyBoards(t, 20, false, "--max-ports", "24") }

// 24 boards join a host server (16 handshake slots) at the same moment.
func TestBoardsAtOnce(t *testing.T) { manyBoards(t, 24, true) }

// 12 boards join at the same moment a server with 2 handshake slots (64
// links: L / 32 = 2), ten more than it takes at once.
func TestBoardsAtOnceTwoSlots(t *testing.T) { manyBoards(t, 12, true, "--max-links", "64") }
