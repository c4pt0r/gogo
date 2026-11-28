package nettest

import (
	"io"
	"testing"

	"gogotools/tests/net/sim"
)

// The keepalive's timings (runtime/net/net.h defaults).
const (
	kaIdle   = 120000 // ms with nothing heard before the first probe
	kaIntvl  = 10000  // ms between probes
	kaProbes = 3      // unanswered probes before the reset
)

// probe checks s is one keepalive probe of c: an ACK without data, one
// below the next sequence number.
func (c *tcpConn) probe(s []sim.TCP) {
	c.t.Helper()
	if len(s) != 1 || s[0].Flags != sim.ACK || s[0].Seq != c.ack-1 || s[0].Ack != c.seq || len(s[0].Payload) != 0 ||
		s[0].SrcPort != c.vport || s[0].DstPort != c.pport {
		c.t.Fatalf("want a keepalive probe (ACK seq %d ack %d), got %s", c.ack-1, c.seq, segsStr(s))
	}
}

// quiet expects no frame from the VM for ms.
func quiet(t *testing.T, v *sim.VM, ms uint32) {
	t.Helper()
	if ms > 0 {
		none(t)(v.Advance(ms))
	}
}

// keepaliveDeath plays a silent peer for each of cs (idle together, the
// first probe due in left ms): probes, then a reset. The frames of the
// last step are returned with the error (io.EOF if the VM then exited).
func keepaliveDeath(t *testing.T, v *sim.VM, left uint32, cs ...*tcpConn) error {
	t.Helper()
	step := func(ms uint32) map[uint16][]sim.TCP {
		frames, err := v.Advance(ms)
		if err != nil && err != io.EOF {
			t.Fatal(err)
		}
		return split(cs[0].segs(frames, nil))
	}
	quiet(t, v, left-1)
	for i := 0; i < kaProbes; i++ {
		m := step(1)
		if len(m) != len(cs) {
			t.Fatalf("probe %d: %v", i+1, m)
		}
		for _, c := range cs {
			c.probe(m[c.pport])
		}
		quiet(t, v, kaIntvl-1)
	}
	frames, err := v.Advance(1)
	m := split(cs[0].segs(frames, nil))
	if len(m) != len(cs) {
		t.Fatalf("want %d resets, got %v", len(cs), m)
	}
	for _, c := range cs {
		c.expect(m[c.pport], sim.RST|sim.ACK, "")
	}
	return err
}

// The issue's repro: three inbound peers connect and vanish, so the
// fourth peer's SYN finds no slot (the last is kept for Dial). After the
// idle time and the unanswered probes the three are reset, their owner
// gets Closed{Timeout}, and the fourth peer is accepted.
func TestTCPKeepaliveDeadPeers(t *testing.T) {
	v := startTCP(t, "tcp_keepalive_listen")
	var cs []*tcpConn
	for i := 0; i < 3; i++ {
		c := newConn(t, v, uint16(40001+i), 80)
		s := c.send(sim.SYN, nil)
		if len(s) != 1 || s[0].Flags != sim.SYN|sim.ACK {
			t.Fatalf("peer %d: want SYN-ACK, got %s", i, segsStr(s))
		}
		c.ack = s[0].Seq + 1
		c.expect(c.send(sim.ACK, nil), sim.ACK, "") // accepted and resumed: window update
		cs = append(cs, c)
	}
	t0 := v.Now()
	late := newConn(t, v, 40004, 80)
	if s := late.send(sim.SYN, nil); len(s) != 0 {
		t.Fatalf("fourth SYN: want no answer (no slot), got %s", segsStr(s))
	}
	if err := keepaliveDeath(t, v, kaIdle, cs...); err != nil {
		t.Fatal(err)
	}
	late.seq-- // the peer resends its SYN
	s := late.send(sim.SYN, nil)
	if len(s) != 1 || s[0].Flags != sim.SYN|sim.ACK {
		t.Fatalf("fourth SYN again: want SYN-ACK, got %s", segsStr(s))
	}
	late.ack = s[0].Seq + 1
	late.expect(late.send(sim.ACK, nil), sim.ACK, "")
	end := itoa(t0 + kaIdle + kaProbes*kaIntvl)
	stop(t, v, "listen true\naccepted 1 "+itoa(t0)+"\naccepted 2 "+itoa(t0)+"\naccepted 3 "+itoa(t0)+"\n"+
		"closed 9 "+end+"\nclosed 9 "+end+"\nclosed 9 "+end+"\naccepted 4 "+end+"\n")
}

// A live peer that answers the probes keeps an idle inbound connection
// open across idle periods; data restarts the idle time.
func TestTCPKeepaliveLivePeer(t *testing.T) {
	v := startTCP(t, "tcp_keepalive_listen")
	c := newConn(t, v, 40001, 80)
	s := c.send(sim.SYN, nil)
	c.ack = s[0].Seq + 1
	c.expect(c.send(sim.ACK, nil), sim.ACK, "")
	for i := 0; i < 4; i++ {
		quiet(t, v, kaIdle-1)
		c.probe(c.segs(v.Advance(1)))
		none(t)(v.Exchange(c.frame(sim.ACK, nil))) // the answer
	}
	// Half an idle period, then data: the idle time starts again from the
	// echo's acknowledgement.
	quiet(t, v, kaIdle/2)
	s = c.send(sim.PSH|sim.ACK, []byte("ping"))
	if len(s) != 2 {
		t.Fatalf("want ACK + echo, got %s", segsStr(s))
	}
	c.expect(s[:1], sim.ACK, "")
	c.expect(s[1:], sim.PSH|sim.ACK, "ping")
	none(t)(v.Exchange(c.frame(sim.ACK, nil)))
	quiet(t, v, kaIdle-1)
	c.probe(c.segs(v.Advance(1)))
	none(t)(v.Exchange(c.frame(sim.ACK, nil)))
	quiet(t, v, kaIntvl*kaProbes) // answered: no second probe, no reset
	stop(t, v, "listen true\naccepted 1 0\n")
}

// An outbound connection: answered probes keep it; data in flight is
// retransmitted as before (no probes meanwhile) and, once acknowledged,
// hands the timer back to the keepalive; data never acknowledged ends
// the connection by the retransmission limit (Closed{Timeout}).
func TestTCPKeepaliveDialRetransmit(t *testing.T) {
	v := startTCP(t, "tcp_keepalive_dial")
	c := newConn(t, v, 80, 0)
	c.accept(c.segs(v.Advance(100)))
	c.expect(c.send(sim.SYN|sim.ACK, nil), sim.ACK, "")
	for i := 0; i < 2; i++ {
		quiet(t, v, kaIdle-1)
		c.probe(c.segs(v.Advance(1)))
		none(t)(v.Exchange(c.frame(sim.ACK, nil)))
	}
	s := c.send(sim.PSH|sim.ACK, []byte("ping"))
	c.expect(s[:1], sim.ACK, "")
	x := c.expect(s[1:], sim.PSH|sim.ACK, "ping")
	c.ack -= 4 // lost: retransmitted after 1 s
	quiet(t, v, 999)
	expectRetx(t, c.segs(v.Advance(1)), x)
	c.ack += 4
	none(t)(v.Exchange(c.frame(sim.ACK, nil))) // Done
	acked := v.Now()
	quiet(t, v, kaIdle-1)
	c.probe(c.segs(v.Advance(1)))
	none(t)(v.Exchange(c.frame(sim.ACK, nil)))
	s = c.send(sim.PSH|sim.ACK, []byte("pong"))
	c.expect(s[:1], sim.ACK, "")
	x = c.expect(s[1:], sim.PSH|sim.ACK, "pong")
	for _, ms := range []uint32{1000, 2000, 4000, 8000, 8000, 8000} {
		quiet(t, v, ms-1)
		expectRetx(t, c.segs(v.Advance(1)), x)
	}
	quiet(t, v, 7999)
	frames, err := v.Advance(1)
	if err == io.EOF {
		err = nil
	}
	s = c.segs(frames, err)
	if len(s) != 1 || s[0].Flags != sim.RST|sim.ACK || s[0].Seq != x.Seq+4 {
		t.Fatalf("want RST, got %s", segsStr(s))
	}
	end := v.Now()
	if _, err := v.Sync(); err != io.EOF {
		t.Fatalf("sync: %v, want EOF", err)
	}
	finish(t, v, "dial true\nconnected 100\ndone 1 "+itoa(acked)+"\ndone -1 "+itoa(end)+"\nclosed 9 "+itoa(end)+"\n")
}

// An outbound connection whose peer vanishes: probes, reset,
// Closed{Timeout} 150 s after the last segment.
func TestTCPKeepaliveDialDeadPeer(t *testing.T) {
	v := startTCP(t, "tcp_keepalive_dial")
	c := newConn(t, v, 80, 0)
	c.accept(c.segs(v.Advance(100)))
	c.expect(c.send(sim.SYN|sim.ACK, nil), sim.ACK, "")
	quiet(t, v, 5000)
	none(t)(v.Exchange(c.frame(sim.ACK, nil))) // the peer's last word
	last := v.Now()
	if err := keepaliveDeath(t, v, kaIdle, c); err != io.EOF {
		t.Fatalf("after the reset: %v, want EOF (the program returned)", err)
	}
	finish(t, v, "dial true\nconnected 100\nclosed 9 "+itoa(last+kaIdle+kaProbes*kaIntvl)+"\n")
}

// flood sends 1000-byte segments until one is refused (the reader's
// quota): it is left unacknowledged, c.seq still before it. Returns the
// refused segment and the bytes acknowledged.
func flood(t *testing.T, v *sim.VM, c *tcpConn) ([]byte, int) {
	t.Helper()
	for got := 0; got < 100000; got += 1000 {
		f := c.frame(sim.PSH|sim.ACK, make([]byte, 1000))
		s := c.segs(v.Exchange(f))
		if len(s) == 0 {
			return f, got
		}
		if len(s) != 1 || s[0].Flags != sim.ACK || s[0].Ack != c.seq+1000 {
			t.Fatalf("want the ACK of %d, got %s", c.seq+1000, segsStr(s))
		}
		c.seq += 1000
	}
	t.Fatal("never refused")
	return nil, 0
}

// The issue's variant: a peer resending into a full mailbox. Refused
// data is not progress, so a reader that never drains loses the
// connection to the keepalive (Closed{Timeout}) even though the peer
// answers the probes; one that drains before the probes run out keeps
// it, though the peer's next resend comes much later.
func TestTCPKeepaliveStalledReader(t *testing.T) {
	v := startTCP(t, "tcp_keepalive_stall")
	c := newConn(t, v, 80, 0)
	c.accept(c.segs(v.Advance(100)))
	c.expect(c.send(sim.SYN|sim.ACK, nil), sim.ACK, "")
	refused, got1 := flood(t, v, c)
	for el := uint32(0); ; {
		frames, err := v.Advance(5000)
		el += 5000
		s := c.segs(frames, err)
		if el == kaIdle+kaProbes*kaIntvl {
			c.expect(s, sim.RST|sim.ACK, "")
			break
		}
		if el >= kaIdle && (el-kaIdle)%kaIntvl == 0 {
			c.probe(s)
			none(t)(v.Exchange(c.frame(sim.ACK, nil))) // answered, but nothing is taken
		} else if len(s) != 0 {
			t.Fatalf("at %d ms: %s", el, segsStr(s))
		}
		none(t)(v.Exchange(refused)) // the resend: refused again
	}
	// The first reader wakes at 400 s, prints, and the second dials.
	c2 := newConn(t, v, 81, 0)
	for i := 0; ; i++ {
		if s := c2.segs(v.Advance(5000)); len(s) != 0 {
			c2.accept(s)
			break
		}
		if i > 100 {
			t.Fatal("no second dial")
		}
	}
	c2.expect(c2.send(sim.SYN|sim.ACK, nil), sim.ACK, "")
	conn2 := v.Now()
	refused, got2 := flood(t, v, c2)
	quiet(t, v, 5000)
	none(t)(v.Exchange(refused))
	quiet(t, v, kaIdle-5000-1)
	c2.probe(c2.segs(v.Advance(1)))
	none(t)(v.Exchange(c2.frame(sim.ACK, nil)))
	// The reader wakes at 125 s and drains; at the next probe's time the
	// stack sees it has room and starts a new idle period instead.
	quiet(t, v, 200000-kaIdle) // the peer's backed-off resend, at 200 s
	s := c2.send(sim.PSH|sim.ACK, refused[len(refused)-1000:])
	c2.expect(s, sim.ACK, "")
	got2 += 1000
	frames, err := v.Exchange(c2.frame(sim.FIN|sim.ACK, nil))
	c2.seq++
	if err == io.EOF {
		err = nil
	}
	s = c2.segs(frames, err)
	if len(s) != 2 {
		t.Fatalf("want ACK + RST, got %s", segsStr(s))
	}
	c2.expect(s[:1], sim.ACK, "")
	c2.expect(s[1:], sim.RST|sim.ACK, "")
	end := v.Now()
	if _, err := v.Sync(); err != io.EOF {
		t.Fatalf("sync: %v, want EOF", err)
	}
	// (The reset came 150 s after the flood; the reader sees it when it wakes.)
	finish(t, v, "dial 80 true\nconnected 100\nawake 400100\ngot "+itoa(uint64(got1))+" closed 9 400100\ndial 81 true\nconnected "+itoa(conn2)+"\nawake "+itoa(conn2+125000)+
		"\ngot "+itoa(uint64(got2))+" closed 0 "+itoa(end)+"\ndone\n")
}
