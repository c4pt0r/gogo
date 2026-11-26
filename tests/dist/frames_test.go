package disttest

// Frame numbering, drops, refusals and the Calls table: each a case a
// review reproduced against an earlier protocol.

import (
	"encoding/binary"
	"fmt"
	"strings"
	"testing"
	"time"
)

// A replayed frame number is refused (no second answer) and the link
// stays; a gap in the numbers is fine.
func TestReplayRefusedGapAccepted(t *testing.T) {
	srv, port := startServer(t)
	p, err := dial(t, port, "mallory", token)
	if err != nil {
		t.Fatal(err)
	}
	echo := p.whereis("echo")
	req, _ := p.request(tStr("once"))
	f := binary.LittleEndian.AppendUint32(hdr(fSend, p.server, p.self), echo)
	f = append(f, req...)
	p.send(f)
	p.sendNum(p.tx, f) // the same number again
	if p.recv(2000) == nil {
		t.Fatal("no answer")
	}
	if r := p.recv(500); r != nil {
		t.Errorf("a replayed frame was answered: %x", r)
	}
	p.tx += 1000 // a gap: frames were lost on the way
	if got := p.call(echo, tStr("after"), 2000); string(got) != string(tStr("after")) {
		t.Errorf("after a gap: %x", got)
	}
	noCrash(t, srv)
}

// A peer that stops reading while the server answers: the server drops
// what it cannot queue, and every frame that does arrive is valid.
func TestSlowReaderKeepsLink(t *testing.T) {
	srv, port := startServer(t)
	p, err := dial(t, port, "slowreader", token)
	if err != nil {
		t.Fatal(err)
	}
	echo := p.whereis("echo")
	big := tStr(strings.Repeat("x", 3000))
	for i := 0; i < 400; i++ {
		req, _ := p.request(big)
		f := binary.LittleEndian.AppendUint32(hdr(fSend, p.server, p.self), echo)
		p.send(append(f, req...))
	}
	time.Sleep(time.Second)
	good := 0
	for {
		r := p.recv(1000) // fails the test on a bad frame
		if r == nil {
			break
		}
		good++
	}
	if got := p.call(echo, tStr("after"), 3000); string(got) != string(tStr("after")) {
		t.Errorf("link unusable after the drops (%d frames read): %x", good, got)
	}
	noCrash(t, srv)
}

// A board's burst beyond its link actor's queue loses messages, not the
// server.
func TestBurstKeepsLink(t *testing.T) {
	srv, port := startServer(t)
	b := start(t, "burst", fmt.Sprintf("test.server=127.0.0.1:%d", port))
	b.waitLine("echo after burst true")
	b.waitLine("end")
	for _, l := range b.output() {
		if strings.HasPrefix(l, "down") {
			t.Errorf("board: %s", l)
		}
	}
	noCrash(t, srv)
}

// A MONITOR of a PID on a node the server cannot reach: the DOWN comes
// back for that node, with a valid MAC (the link's own ids).
func TestMonitorUnroutable(t *testing.T) {
	srv, port := startServer(t)
	p, err := dial(t, port, "mallory", token)
	if err != nil {
		t.Fatal(err)
	}
	f := hdr(6, 999, p.self) // MONITOR, to node 999
	f = binary.LittleEndian.AppendUint32(f, p.calledBy)
	f = binary.LittleEndian.AppendUint32(f, 0x1236)
	f = binary.LittleEndian.AppendUint64(f, 77)
	p.send(f)
	r := p.recv(2000)
	if r == nil || r[0] != 8 || binary.LittleEndian.Uint16(r[4:]) != 999 {
		t.Fatalf("want DOWN from node 999, got %x", r)
	}
	if p.whereis("echo") == 0 {
		t.Error("link unusable after the DOWN")
	}
	noCrash(t, srv)
}

// A LINK to the door is refused: dropping the connection afterwards
// leaves the node serving new boards.
func TestLinkToDoorRefused(t *testing.T) {
	srv, port := startServer(t)
	p, err := dial(t, port, "mallory", token)
	if err != nil {
		t.Fatal(err)
	}
	f := hdr(3, p.server, p.self) // LINK from our PID to the door
	f = binary.LittleEndian.AppendUint32(f, p.calledBy)
	f = binary.LittleEndian.AppendUint32(f, 0)
	p.send(f)
	if r := p.recv(2000); r == nil || r[0] != 5 {
		t.Errorf("want EXIT (refused), got %x", r)
	}
	p.c.Close()
	srv.waitLine("down mallory")
	q, err := dial(t, port, "honest", token)
	if err != nil {
		t.Fatalf("a new board cannot join: %v", err)
	}
	if q.whereis("echo") == 0 {
		t.Error("echo not found")
	}
	srv.kill()
	if strings.Contains(srv.errs, "crashed") {
		t.Errorf("server stderr:\n%s", srv.errs)
	}
}
