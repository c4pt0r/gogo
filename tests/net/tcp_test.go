package nettest

import (
	"io"
	"strconv"
	"strings"
	"testing"

	"gogotools/tests/net/sim"
)

func alpha(n int) string {
	var b strings.Builder
	for i := 0; i < n; i++ {
		b.WriteByte(byte('a' + i%26))
	}
	return b.String()
}

// Active open, segmentation by the peer's MSS, one segment in flight,
// Done per Out, FIN after the writes, duplicate and out-of-order
// segments re-acknowledged and not delivered, active close.
func TestTCPDial(t *testing.T) {
	v := startTCP(t, "tcp_dial")
	c := newConn(t, v, 80, 0)
	c.accept(c.segs(v.Advance(100)))
	s := c.send(sim.SYN|sim.ACK, nil, sim.MSSOption(1000)...)
	if len(s) != 2 {
		t.Fatalf("want ACK + data, got %s", segsStr(s))
	}
	if s[0].Window != 1460 {
		t.Fatalf("window %d", s[0].Window)
	}
	c.expect(s[:1], sim.ACK, "")
	data := alpha(3000)
	c.expect(s[1:], sim.PSH|sim.ACK, data[:1000])
	c.expect(c.send(sim.ACK, nil), sim.PSH|sim.ACK, data[1000:2000])
	c.expect(c.send(sim.ACK, nil), sim.PSH|sim.ACK, data[2000:])
	c.ack -= 1000 // a duplicate (old) ACK: nothing to send
	none(t)(v.Exchange(c.frame(sim.ACK, nil)))
	c.ack += 1000
	c.expect(c.send(sim.ACK, nil), sim.FIN|sim.PSH|sim.ACK, "world")
	c.expect(c.send(sim.PSH|sim.ACK, []byte("reply")), sim.ACK, "")
	// The same segment again: re-acknowledged, not delivered twice.
	c.seq -= 5
	c.expect(c.send(sim.PSH|sim.ACK, []byte("reply")), sim.ACK, "")
	// Out of order (a gap): dropped, the expected number acknowledged.
	c.seq += 100
	f := c.frame(sim.PSH|sim.ACK, []byte("zzz"))
	c.seq -= 100
	c.expect(c.segs(v.Exchange(f)), sim.ACK, "")
	// The peer's FIN: acknowledged, Closed{Normal}, the program exits
	// (TIME_WAIT does not keep the VM running).
	frames, err := v.Exchange(c.frame(sim.FIN|sim.ACK, nil))
	c.seq++
	if err == io.EOF {
		err = nil
	}
	c.expect(c.segs(frames, err), sim.ACK, "")
	if _, err := v.Sync(); err != io.EOF {
		t.Fatalf("sync: %v, want EOF", err)
	}
	finish(t, v, "dial true\nconnected\nwritten\nwritten\ndata reply\nclosed 0\n")
}

// Passive open: RST for a port nobody listens on; an accepted connection
// is paused (window 0, data not delivered) until handed off; then the
// handler echoes and closes.
func TestTCPListenHandoff(t *testing.T) {
	v := startTCP(t, "tcp_listen_handoff")
	// No listener on 9999: RST|ACK acknowledging the SYN.
	stray := newConn(t, v, 41000, 9999)
	s := stray.send(sim.SYN, nil)
	if len(s) != 1 || s[0].Flags != sim.RST|sim.ACK || s[0].Seq != 0 || s[0].Ack != stray.seq {
		t.Fatalf("want RST|ACK, got %s", segsStr(s))
	}
	c := newConn(t, v, 40000, 8080)
	s = c.send(sim.SYN, nil, sim.MSSOption(1460)...)
	if len(s) != 1 || s[0].Flags != sim.SYN|sim.ACK || s[0].Ack != c.seq || s[0].Window != 0 {
		t.Fatalf("want SYN|ACK with window 0, got %s", segsStr(s))
	}
	if m, ok := s[0].MSS(); !ok || m != 1460 {
		t.Fatalf("SYN-ACK MSS %d %v", m, ok)
	}
	c.ack = s[0].Seq + 1
	none(t)(v.Exchange(c.frame(sim.ACK, nil))) // established, Accepted, paused
	// Data while paused: not delivered, not acknowledged, window 0.
	x := c.expect(c.segs(v.Exchange(c.frame(sim.PSH|sim.ACK, []byte("early")))), sim.ACK, "")
	if x.Window != 0 {
		t.Fatalf("paused window %d", x.Window)
	}
	// Handoff: a window update.
	x = c.expect(c.segs(v.Advance(100)), sim.ACK, "")
	if x.Window != 1460 {
		t.Fatalf("window after handoff %d", x.Window)
	}
	s = c.send(sim.PSH|sim.ACK, []byte("early")) // the peer resends
	if len(s) != 2 {
		t.Fatalf("want ACK + echo, got %s", segsStr(s))
	}
	c.expect(s[:1], sim.ACK, "")
	c.expect(s[1:], sim.PSH|sim.ACK, "early")
	c.expect(c.send(sim.ACK, nil), sim.FIN|sim.ACK, "")
	frames, err := v.Exchange(c.frame(sim.FIN|sim.ACK, nil))
	c.seq++
	if err == io.EOF {
		err = nil
	}
	c.expect(c.segs(frames, err), sim.ACK, "")
	if _, err := v.Sync(); err != io.EOF {
		t.Fatalf("sync: %v, want EOF", err)
	}
	finish(t, v, "listen true\nagain false\naccepted\nhandoff true\ngot early\nwritten\nclosed 0\nhandler done\n")
}

// expectRetx checks s is one retransmission of x.
func expectRetx(t *testing.T, s []sim.TCP, x sim.TCP) {
	t.Helper()
	if len(s) != 1 || s[0].Flags != x.Flags || s[0].Seq != x.Seq || string(s[0].Payload) != string(x.Payload) {
		t.Fatalf("want a retransmission of %s, got %s", segStr(x), segsStr(s))
	}
}

// Retransmission after 1 s doubling to 8 s, of the SYN and of data; six
// retransmissions, then RST and Closed{Timeout}; Dial's TimeoutMs.
func TestTCPRetransmit(t *testing.T) {
	v := startTCP(t, "tcp_retransmit")
	c := newConn(t, v, 80, 0)
	syn := c.accept(c.segs(v.Advance(100)))
	none := func(ms uint32) {
		t.Helper()
		if s := c.segs(v.Advance(ms)); len(s) != 0 {
			t.Fatalf("early: %s", segsStr(s))
		}
	}
	none(999)
	expectRetx(t, c.segs(v.Advance(1)), syn)
	none(1999)
	expectRetx(t, c.segs(v.Advance(1)), syn)
	s := c.send(sim.SYN|sim.ACK, nil)
	c.expect(s[:1], sim.ACK, "")
	x := c.expect(s[1:], sim.PSH|sim.ACK, "abc")
	c.ack -= 3
	expectRetx(t, c.segs(v.Advance(1000)), x)
	expectRetx(t, c.segs(v.Advance(2000)), x)
	c.ack += 3
	x = c.expect(c.send(sim.ACK, nil), sim.PSH|sim.ACK, "def") // Done, then "def"
	for _, ms := range []uint32{1000, 2000, 4000, 8000, 8000, 8000} {
		none(ms - 1)
		expectRetx(t, c.segs(v.Advance(1)), x)
	}
	none(7999)
	s = c.segs(v.Advance(1))
	if len(s) != 2 || s[0].Flags != sim.RST|sim.ACK || s[0].Seq != x.Seq+3 || s[1].Flags != sim.SYN || s[1].DstPort != 81 {
		t.Fatalf("want RST then a SYN to 81, got %s", segsStr(s))
	}
	c2 := newConn(t, v, 81, 0)
	syn = c2.accept(s[1:])
	expectRetx(t, c2.segs(v.Advance(1000)), syn)
	s = c2.segs(v.Advance(1499))
	if len(s) != 0 {
		t.Fatalf("early: %s", segsStr(s))
	}
	if _, err := v.Advance(1); err != io.EOF {
		t.Fatalf("advance: %v, want EOF", err)
	}
	finish(t, v, "dial true\nconnected\nwritten\nnot sent\nclosed 9 45100\ndial2 true\nclosed 9 47600\n")
}

// A slow reader near its quota: segments are not acknowledged until it
// has room, the peer resends, every byte arrives once and in order, and
// the reader is never killed.
func TestTCPBackpressure(t *testing.T) {
	v := startTCP(t, "tcp_backpressure")
	c := newConn(t, v, 80, 0)
	c.accept(c.segs(v.Advance(100)))
	c.expect(c.send(sim.SYN|sim.ACK, nil), sim.ACK, "")
	const total, seg = 10000, 1000
	payload := func(off int) []byte {
		b := make([]byte, seg)
		for i := range b {
			b[i] = byte((off + i) % 251)
		}
		return b
	}
	refused, sent := 0, 0
	for off := 0; off < total; {
		s := c.segs(v.Exchange(c.frame(sim.PSH|sim.ACK, payload(off))))
		sent++
		if len(s) == 0 {
			refused++
			if s := c.segs(v.Advance(500)); len(s) != 0 { // the peer's RTO
				t.Fatalf("unexpected %s", segsStr(s))
			}
			continue
		}
		if len(s) != 1 || s[0].Flags != sim.ACK || s[0].Ack != c.seq+seg {
			t.Fatalf("want the ACK of %d, got %s", c.seq+seg, segsStr(s))
		}
		c.seq += seg
		off += seg
	}
	if refused == 0 || sent > 200 {
		t.Fatalf("%d segments sent, %d not acknowledged: want some back-pressure", sent, refused)
	}
	t.Logf("%d segments sent for %d, %d not acknowledged", sent, total/seg, refused)
	frames, err := v.Exchange(c.frame(sim.FIN|sim.ACK, nil))
	c.seq++
	if err == io.EOF {
		err = nil
	}
	s := c.segs(frames, err)
	if len(s) != 2 {
		t.Fatalf("want ACK + RST (closed without Shutdown), got %s", segsStr(s))
	}
	c.expect(s[:1], sim.ACK, "")
	c.expect(s[1:], sim.RST|sim.ACK, "")
	if _, err := v.Sync(); err != io.EOF {
		t.Fatalf("sync: %v, want EOF", err)
	}
	finish(t, v, "dial true\nconnected\ngot 10000 bad 0 closed 0\ndone\n")
}

// RST mid-transfer (Closed{Reset}), RST to a SYN (Closed{Refused}), a
// SYN-ACK acknowledging the wrong number (answered with RST), an
// out-of-window RST (ignored), segments for a gone connection (RST).
func TestTCPRst(t *testing.T) {
	v := startTCP(t, "tcp_rst")
	a := newConn(t, v, 80, 0)
	a.accept(a.segs(v.Advance(100)))
	s := a.send(sim.SYN|sim.ACK, nil)
	a.expect(s[:1], sim.ACK, "")
	a.expect(s[1:], sim.PSH|sim.ACK, "x")
	s = a.segs(v.Exchange(a.frame(sim.RST, nil))) // Closed{Reset}; the program dials 81
	b := newConn(t, v, 81, 0)
	b.accept(s)
	// A's connection is gone: its segments get RST.
	s = a.send(sim.ACK, nil)
	if len(s) != 1 || s[0].Flags != sim.RST || s[0].Seq != a.ack {
		t.Fatalf("want RST seq %d, got %s", a.ack, segsStr(s))
	}
	// SYN-ACK acknowledging the wrong number: RST with that number.
	bad := sim.TCPFrame(gwHost, vmHost, sim.TCP{SrcPort: 81, DstPort: b.vport, Seq: b.seq, Ack: 12345,
		Flags: sim.SYN | sim.ACK, Window: 8192})
	s = b.segs(v.Exchange(bad))
	if len(s) != 1 || s[0].Flags != sim.RST || s[0].Seq != 12345 {
		t.Fatalf("want RST seq 12345, got %s", segsStr(s))
	}
	s = b.segs(v.Exchange(b.frame(sim.RST|sim.ACK, nil))) // refused; the program dials 82
	c := newConn(t, v, 82, 0)
	c.accept(s)
	s = c.send(sim.SYN|sim.ACK, nil)
	c.expect(s[:1], sim.ACK, "")
	c.expect(s[1:], sim.PSH|sim.ACK, "y")
	c.seq += 50000 // out of window: ignored
	none(t)(v.Exchange(c.frame(sim.RST, nil)))
	c.seq -= 50000
	none(t)(v.Exchange(c.frame(sim.ACK, nil))) // Done: still connected
	if _, err := v.Exchange(c.frame(sim.RST, nil)); err != io.EOF {
		t.Fatalf("exchange: %v, want EOF", err)
	}
	finish(t, v, "connected\nnot sent\nclosed 19\nclosed 18\nconnected\nwritten\nclosed 19\n")
}

// A handler that shuts down and exits leaves an orphan: the response is
// retransmitted after its owner is gone and the close completes.
func TestTCPOrphanClose(t *testing.T) {
	v := startTCP(t, "tcp_orphan_close")
	c := newConn(t, v, 40000, 8080)
	s := c.send(sim.SYN, nil)
	c.ack = s[0].Seq + 1
	c.expect(c.send(sim.ACK, nil), sim.ACK, "") // accepted, handed off: window update
	s = c.send(sim.PSH|sim.ACK, []byte("GET"))
	if len(s) != 2 {
		t.Fatalf("want ACK + response, got %s", segsStr(s))
	}
	c.expect(s[:1], sim.ACK, "")
	x := c.expect(s[1:], sim.PSH|sim.ACK, "response")
	expectRetx(t, c.segs(v.Advance(1000)), x) // the owner has exited
	c.expect(c.send(sim.ACK, nil), sim.FIN|sim.ACK, "")
	none(t)(v.Exchange(c.frame(sim.ACK, nil)))
	c.expect(c.send(sim.FIN|sim.ACK, nil), sim.ACK, "") // TIME_WAIT
	c.seq--
	c.expect(c.send(sim.FIN|sim.ACK, nil), sim.ACK, "") // a resent FIN: ACKed again
	if _, err := v.Advance(2000); err != io.EOF {
		t.Fatalf("advance: %v, want EOF", err)
	}
	finish(t, v, "accepted\nhandler exit 0\n")
}

// An orphan's life is bounded: a peer with a 1-byte window
// that acknowledges every byte just before the retransmission timer keeps
// it progressing, but it is reset 20 s after its owner exited, its data
// released; the program carries on.
func TestTCPOrphanLifetime(t *testing.T) {
	v := startTCP(t, "tcp_orphan_slow")
	c := newConn(t, v, 40000, 8080)
	s := c.send(sim.SYN, nil)
	c.ack = s[0].Seq + 1
	c.expect(c.send(sim.ACK, nil), sim.ACK, "") // accepted, handed off: window update
	c.wnd = 1
	s = c.send(sim.PSH|sim.ACK, []byte("GET")) // the handler answers and exits: an orphan
	if len(s) != 2 {
		t.Fatalf("want ACK + 1 byte, got %s", segsStr(s))
	}
	orphaned := v.Now()
	c.expect(s[:1], sim.ACK, "")
	c.expect(s[1:], sim.PSH|sim.ACK, "z")
	bytes := 1
	for {
		s = c.segs(v.Advance(900)) // the 1 s retransmission never comes
		if len(s) == 1 && s[0].Flags == sim.RST|sim.ACK {
			break
		}
		if len(s) != 0 {
			t.Fatalf("at %d ms: want nothing or RST, got %s", v.Now()-orphaned, segsStr(s))
		}
		c.expect(c.send(sim.ACK, nil), sim.PSH|sim.ACK, "z")
		bytes++
	}
	if d := v.Now() - orphaned; d < 20000 || d >= 20900 || bytes < 20 || bytes >= 200 {
		t.Fatalf("RST after %d ms and %d bytes, want at 20 s while progressing", d, bytes)
	}
	if s = c.send(sim.ACK, nil); len(s) != 1 || s[0].Flags != sim.RST { // gone: no connection
		t.Fatalf("want RST, got %s", segsStr(s))
	}
	if _, err := v.Advance(10000); err != io.EOF {
		t.Fatalf("advance: %v, want EOF", err)
	}
	finish(t, v, "accepted\nhandler exit 0\n")
}

// split sorts segments by destination port.
func split(s []sim.TCP) map[uint16][]sim.TCP {
	m := map[uint16][]sim.TCP{}
	for _, x := range s {
		m[x.DstPort] = append(m[x.DstPort], x)
	}
	return m
}

// An owner crashing with its connection in each state: RST (or nothing
// in SYN_SENT), or, after Shutdown, an orphan that completes the close or
// gives up with RST; the survivor is unaffected; every slot comes back.
func TestTCPOwnerCrashStates(t *testing.T) {
	v := startTCP(t, "owner_crash_states")
	sv := newConn(t, v, 2000, 0)
	sv.accept(sv.segs(v.Advance(100)))
	m := split(sv.send(sim.SYN|sim.ACK, nil))
	sv.expect(m[2000], sim.ACK, "")
	// 1: SYN_SENT. Nothing sent at the crash; the late SYN-ACK gets RST.
	c1 := newConn(t, v, 1001, 0)
	c1.accept(m[1001])
	c2 := newConn(t, v, 1002, 0)
	c2.accept(c2.segs(v.Advance(500)))
	s := c1.send(sim.SYN|sim.ACK, nil)
	if len(s) != 1 || s[0].Flags != sim.RST || s[0].Seq != c1.ack {
		t.Fatalf("want RST, got %s", segsStr(s))
	}
	// 2: ESTABLISHED: RST.
	c2.expect(c2.send(sim.SYN|sim.ACK, nil), sim.ACK, "")
	m = split(c2.segs(v.Advance(500)))
	c2.expect(m[1002], sim.RST|sim.ACK, "")
	// 3: CLOSE_WAIT: RST.
	c3 := newConn(t, v, 1003, 0)
	c3.accept(m[1003])
	c3.expect(c3.send(sim.SYN|sim.ACK, nil), sim.ACK, "")
	c3.expect(c3.send(sim.FIN|sim.ACK, nil), sim.ACK, "")
	c3.expect(c3.segs(v.Advance(500)), sim.RST|sim.ACK, "")
	// 4: accepted and paused: RST; the listener goes too.
	c4 := newConn(t, v, 45000, 1004)
	s = c4.send(sim.SYN, nil)
	if len(s) != 1 || s[0].Flags != sim.SYN|sim.ACK {
		t.Fatalf("want SYN-ACK, got %s", segsStr(s))
	}
	c4.ack = s[0].Seq + 1
	none(t)(v.Exchange(c4.frame(sim.ACK, nil)))
	m = split(c4.segs(v.Advance(500)))
	c4.expect(m[45000], sim.RST|sim.ACK, "")
	// 5: FIN_WAIT_1 with data in flight: the orphan resends it, then FIN.
	c5 := newConn(t, v, 1005, 0)
	c5.accept(m[1005])
	s = c5.send(sim.SYN|sim.ACK, nil)
	if len(s) != 2 {
		t.Fatalf("want ACK + data, got %s", segsStr(s))
	}
	c5.expect(s[:1], sim.ACK, "")
	x := c5.expect(s[1:], sim.PSH|sim.ACK, "bye")
	c6 := newConn(t, v, 1006, 0)
	c6.accept(c6.segs(v.Advance(500))) // owner 5 exits, owner 6 dials
	expectRetx(t, c5.segs(v.Advance(500)), x)
	c5.expect(c5.send(sim.ACK, nil), sim.FIN|sim.ACK, "")
	none(t)(v.Exchange(c5.frame(sim.ACK, nil)))
	c5.expect(c5.send(sim.FIN|sim.ACK, nil), sim.ACK, "") // TIME_WAIT
	// 6: FIN_WAIT_2: the orphan gives up 4 s later with RST.
	s = c6.send(sim.SYN|sim.ACK, nil)
	if len(s) != 2 {
		t.Fatalf("want ACK + FIN, got %s", segsStr(s))
	}
	c6.expect(s[:1], sim.ACK, "")
	c6.expect(s[1:], sim.FIN|sim.ACK, "")
	none(t)(v.Exchange(c6.frame(sim.ACK, nil)))
	c7 := newConn(t, v, 1007, 0)
	c7.accept(c7.segs(v.Advance(500))) // owner 6 exits, owner 7 dials
	// 7: LAST_ACK: the orphan resends its FIN until acknowledged.
	c7.expect(c7.send(sim.SYN|sim.ACK, nil), sim.ACK, "")
	s = c7.send(sim.FIN|sim.ACK, nil)
	if len(s) != 2 {
		t.Fatalf("want ACK + FIN, got %s", segsStr(s))
	}
	c7.expect(s[:1], sim.ACK, "")
	fin := c7.expect(s[1:], sim.FIN|sim.ACK, "")
	none(t)(v.Advance(500)) // owner 7 exits
	expectRetx(t, c7.segs(v.Advance(500)), fin)
	none(t)(v.Exchange(c7.frame(sim.ACK, nil)))
	none(t)(v.Advance(2999))
	c6.expect(c6.segs(v.Advance(1)), sim.RST|sim.ACK, "")
	// The survivor still works; then three dials fill the freed slots and
	// main returns (the survivor is reset).
	sv.expect(sv.segs(v.Advance(1500)), sim.PSH|sim.ACK, "alive")
	frames, err := v.Exchange(sv.frame(sim.ACK, nil))
	if err == io.EOF {
		err = nil
	}
	m = split(sv.segs(frames, err))
	for p := uint16(3001); p <= 3003; p++ {
		if len(m[p]) != 1 || m[p][0].Flags != sim.SYN {
			t.Fatalf("want a SYN to %d, got %s", p, segsStr(m[p]))
		}
	}
	if len(m[2000]) != 0 {
		t.Fatalf("survivor: %s", segsStr(m[2000]))
	}
	frames, err = v.Advance(10)
	if err != io.EOF {
		t.Fatalf("advance: %v, want EOF", err)
	}
	sv.expect(sv.segs(frames, nil), sim.RST|sim.ACK, "")
	if _, err := v.Sync(); err != io.EOF {
		t.Fatalf("sync: %v, want EOF", err)
	}
	finish(t, v, "survivor up\nowner 1 exit 9\nowner 2 exit 9\nowner 3 exit 9\nlisten true\nowner 4 exit 9\nowner 5 exit 9\n"+
		"owner 6 exit 9\nowner 7 exit 9\nsurvivor written\ndial true\ndial true\ndial true\n")
}

// More SYNs than slots: the extra ones are dropped (no answer); an active
// open takes a half-open connection's slot (RST); half-open connections
// give up after the retransmission limit (RST) and free their slots.
func TestTCPSynFlood(t *testing.T) {
	v := startTCP(t, "syn_flood")
	var cs []*tcpConn
	for i := 0; i < 6; i++ {
		c := newConn(t, v, uint16(50001+i), 80)
		s := c.send(sim.SYN, nil)
		if i < 3 { // the 4th would take the last free slot, kept for Dial
			if len(s) != 1 || s[0].Flags != sim.SYN|sim.ACK || s[0].Ack != c.seq {
				t.Fatalf("SYN %d: want SYN-ACK, got %s", i, segsStr(s))
			}
			c.ack = s[0].Seq + 1
		} else if len(s) != 0 {
			t.Fatalf("SYN %d: no slot, want nothing, got %s", i, segsStr(s))
		}
		cs = append(cs, c)
	}
	// The first completes: Accepted; the program dials out twice: the
	// free slot, then a half-open connection's (silently); that peer's
	// ACK then gets RST.
	s := cs[0].segs(v.Exchange(cs[0].frame(sim.ACK, nil)))
	m := split(s)
	if len(m[90]) != 1 || m[90][0].Flags != sim.SYN || len(m[91]) != 1 || m[91][0].Flags != sim.SYN || len(s) != 2 {
		t.Fatalf("want SYNs to 90 and 91, got %s", segsStr(s))
	}
	s = cs[1].send(sim.ACK, nil)
	if len(s) != 1 || s[0].Flags != sim.RST || s[0].Seq != cs[1].ack {
		t.Fatalf("evicted half-open: want RST, got %s", segsStr(s))
	}
	// 39 s: six retransmissions each, then RST (half-open) or Closed{Timeout}.
	m = split(cs[0].segs(v.Advance(39000)))
	for _, p := range []uint16{50003} {
		if len(m[p]) != 7 || m[p][5].Flags != sim.SYN|sim.ACK || m[p][6].Flags != sim.RST|sim.ACK {
			t.Fatalf("port %d: want 6 SYN-ACK + RST, got %s", p, segsStr(m[p]))
		}
	}
	if len(m[90]) != 6 || len(m[91]) != 6 || len(m) != 3 {
		t.Fatalf("want 6 SYNs each to 90 and 91 and nothing else, got %v", m)
	}
	// Slots are free again.
	c := newConn(t, v, 50007, 80)
	s = c.send(sim.SYN, nil)
	if len(s) != 1 || s[0].Flags != sim.SYN|sim.ACK {
		t.Fatalf("want SYN-ACK, got %s", segsStr(s))
	}
	c.ack = s[0].Seq + 1
	frames, err := v.Exchange(c.frame(sim.ACK, nil))
	if err != io.EOF {
		t.Fatalf("exchange: %v, want EOF", err)
	}
	// main returns: its netif port closes first (the address goes, every
	// connection ends without a segment), then the rest.
	if s := c.segs(frames, nil); len(s) != 0 {
		t.Fatalf("want nothing, got %s", segsStr(s))
	}
	finish(t, v, "listen true\naccepted\ndial true\ndial true\nclosed 9\nclosed 9\naccepted\n")
}

// TIME_WAIT: the peer's resent FIN is acknowledged for 2 s, then answered
// with RST; with every slot in TIME_WAIT a dial reclaims the oldest.
func TestTCPTimeWaitReclaim(t *testing.T) {
	v := startTCP(t, "time_wait_reclaim")
	// closeActive runs one connection to TIME_WAIT.
	closeActive := func(c *tcpConn, s []sim.TCP) {
		t.Helper()
		c.accept(s)
		s = c.send(sim.SYN|sim.ACK, nil)
		if len(s) != 2 {
			t.Fatalf("want ACK + FIN, got %s", segsStr(s))
		}
		c.expect(s[:1], sim.ACK, "")
		c.expect(s[1:], sim.FIN|sim.ACK, "")
	}
	c := newConn(t, v, 100, 0)
	closeActive(c, c.segs(v.Advance(100)))
	none(t)(v.Exchange(c.frame(sim.ACK, nil)))
	c.expect(c.send(sim.FIN|sim.ACK, nil), sim.ACK, "") // TIME_WAIT, Closed{Normal}
	v.Advance(1999)
	c.seq--
	c.expect(c.send(sim.FIN|sim.ACK, nil), sim.ACK, "")
	v.Advance(1)
	c.seq--
	s := c.send(sim.FIN|sim.ACK, nil)
	if len(s) != 1 || s[0].Flags != sim.RST || s[0].Seq != c.ack {
		t.Fatalf("after TIME_WAIT: want RST, got %s", segsStr(s))
	}
	// Four more into TIME_WAIT (each dial after the previous Closed).
	next := c.segs(v.Advance(1000))
	var first *tcpConn
	for p := uint16(101); p <= 104; p++ {
		c := newConn(t, v, p, 0)
		if first == nil {
			first = c
		}
		closeActive(c, next)
		none(t)(v.Exchange(c.frame(sim.ACK, nil)))
		next = c.segs(v.Exchange(c.frame(sim.FIN|sim.ACK, nil)))
		c.seq++
		c.expect(next[:1], sim.ACK, "")
		next = next[1:]
		v.Advance(10)
	}
	// The fifth dial took the oldest TIME_WAIT slot (port 101's): its FIN
	// is now answered with RST, port 102's still with ACK.
	c5 := newConn(t, v, 105, 0)
	c5.accept(next)
	first.seq--
	s = first.send(sim.FIN|sim.ACK, nil)
	if len(s) != 1 || s[0].Flags != sim.RST {
		t.Fatalf("reclaimed: want RST, got %s", segsStr(s))
	}
	if _, err := v.Advance(100); err != io.EOF {
		t.Fatalf("advance: %v, want EOF", err)
	}
	finish(t, v, "closed 0\nclosed 0\nclosed 0\nclosed 0\nclosed 0\ndial true\n")
}

// Slow readers whose mailboxes are full to the last byte still get
// Done and the peer's reset, and are not killed (spec §9.1: control
// events never overflow them).
func TestTCPSlowReaderReset(t *testing.T) {
	v := startTCP(t, "tcp_slow_reader_reset")
	var want strings.Builder
	for r := 0; r < 10; r++ {
		c := newConn(t, v, 80, 0)
		c.accept(c.segs(v.Advance(100)))
		s := c.send(sim.SYN|sim.ACK, nil)
		c.expect(s[:1], sim.ACK, "")
		c.expect(s[1:], sim.PSH|sim.ACK, strings.Repeat("r", 100))
		c.ack -= 100 // the request stays unacknowledged while the peer sends
		got := 0
		for _, size := range []int{1000, 300, 100, 30, 10, 3, 1} {
			for i := 0; ; i++ {
				s := c.segs(v.Exchange(c.frame(sim.PSH|sim.ACK, make([]byte, size))))
				if len(s) == 0 {
					break // refused: no room for this size
				}
				if i > 50 || len(s) != 1 || s[0].Ack != c.seq+uint32(size) {
					t.Fatalf("want the ACK of %d, got %s", c.seq+uint32(size), segsStr(s))
				}
				c.seq += uint32(size)
				got += size
			}
		}
		c.ack += 100
		none(t)(v.Exchange(c.frame(sim.ACK, nil))) // Done
		none(t)(v.Exchange(c.frame(sim.RST, nil))) // Closed{Reset}
		if _, err := v.Advance(3000); err != nil && r < 9 {
			t.Fatalf("advance: %v", err)
		}
		want.WriteString("got " + strconv.Itoa(got) + " written 1 closed 19 req 100\n")
	}
	if _, err := v.Sync(); err != io.EOF {
		t.Fatalf("sync: %v, want EOF", err)
	}
	want.WriteString("done\n")
	finish(t, v, want.String())
}

// Zero-window persist (RFC 1122 4.2.2.17): a peer that keeps its window
// closed for 5 minutes but answers every 1-byte probe keeps the
// connection (probes back off to 8 s and go on); when it opens the
// window the transfer completes.
func TestTCPZeroWindowPersist(t *testing.T) {
	v := startTCP(t, "tcp_keepalive_dial")
	c := newConn(t, v, 80, 0)
	c.accept(c.segs(v.Advance(100)))
	c.expect(c.send(sim.SYN|sim.ACK, nil), sim.ACK, "")
	c.wnd = 0
	s := c.send(sim.PSH|sim.ACK, []byte("ping")) // echoed into a closed window
	if len(s) != 2 {
		t.Fatalf("want ACK + a 1-byte probe, got %s", segsStr(s))
	}
	c.expect(s[:1], sim.ACK, "")
	x := s[1]
	if x.Flags != sim.PSH|sim.ACK || string(x.Payload) != "p" || x.Seq != c.ack {
		t.Fatalf("want a 1-byte probe, got %s", segStr(x))
	}
	start := v.Now()
	gap := uint32(1000)
	for v.Now()-start < 300000 {
		quiet(t, v, gap-1)
		expectRetx(t, c.segs(v.Advance(1)), x)
		none(t)(v.Exchange(c.frame(sim.ACK, nil))) // answered: window still 0
		if gap < 8000 {
			gap *= 2
		}
	}
	c.wnd = 8192
	none(t)(v.Exchange(c.frame(sim.ACK, nil))) // the window opens
	quiet(t, v, gap-1)
	expectRetx(t, c.segs(v.Advance(1)), x)
	c.ack++
	c.expect(c.send(sim.ACK, nil), sim.PSH|sim.ACK, "ing")
	none(t)(v.Exchange(c.frame(sim.ACK, nil))) // Done
	stop(t, v, "dial true\nconnected 100\ndone 1 "+itoa(v.Now())+"\n")
}
