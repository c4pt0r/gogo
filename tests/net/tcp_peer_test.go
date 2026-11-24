package nettest

import (
	"bytes"
	"strconv"
	"testing"

	"gogo/tests/net/sim"
)

// tcpConn is the simulator's end of one TCP connection with the VM.
type tcpConn struct {
	t            *testing.T
	v            *sim.VM
	peer         sim.Host
	pport, vport uint16
	seq          uint32 // the peer's next sequence number
	ack          uint32 // the next byte expected from the VM
	wnd          uint16
}

const peerISN = 70000

func newConn(t *testing.T, v *sim.VM, pport, vport uint16) *tcpConn {
	return &tcpConn{t: t, v: v, peer: gwHost, pport: pport, vport: vport, seq: peerISN, wnd: 8192}
}

// frame is a segment from the peer with the connection's current numbers.
func (c *tcpConn) frame(flags uint8, payload []byte, opts ...byte) []byte {
	return sim.TCPFrame(c.peer, vmHost, sim.TCP{SrcPort: c.pport, DstPort: c.vport, Seq: c.seq, Ack: c.ack,
		Flags: flags, Window: c.wnd, Options: opts, Payload: payload})
}

// send sends a segment and advances the peer's sequence number past it;
// the VM's answer, once idle, is returned as segments of this connection.
func (c *tcpConn) send(flags uint8, payload []byte, opts ...byte) []sim.TCP {
	c.t.Helper()
	f := c.frame(flags, payload, opts...)
	c.seq += uint32(len(payload))
	if flags&(sim.SYN|sim.FIN) != 0 {
		c.seq++
	}
	return c.segs(c.v.Exchange(f))
}

// segs decodes frames from the VM: every one must be TCP to the peer.
func (c *tcpConn) segs(frames [][]byte, err error) []sim.TCP {
	c.t.Helper()
	if err != nil {
		c.t.Fatal(err)
	}
	var out []sim.TCP
	for _, f := range frames {
		p := decode(c.t, f)
		if p.TCP == nil || p.IP.Src != vmHost.IP || p.IP.Dst != c.peer.IP || p.Eth.Dst != c.peer.MAC {
			c.t.Fatalf("want TCP to %v, got %+v %+v", c.peer.IP, p.Eth, p.IP)
		}
		out = append(out, *p.TCP)
	}
	return out
}

// mine keeps the segments of this connection (by port pair).
func (c *tcpConn) mine(s []sim.TCP) []sim.TCP {
	var out []sim.TCP
	for _, x := range s {
		if x.SrcPort == c.vport && x.DstPort == c.pport {
			out = append(out, x)
		}
	}
	return out
}

// expect checks s is exactly one segment of this connection with flags,
// the peer's numbers acknowledged and payload; the payload (and a FIN) are
// then counted as received.
func (c *tcpConn) expect(s []sim.TCP, flags uint8, payload string) sim.TCP {
	c.t.Helper()
	if len(s) != 1 {
		c.t.Fatalf("%d segments, want 1 (%s %q): %s", len(s), flagStr(flags), payload, segsStr(s))
	}
	x := s[0]
	if x.SrcPort != c.vport || x.DstPort != c.pport || x.Flags != flags || x.Seq != c.ack ||
		(flags&sim.ACK != 0 && x.Ack != c.seq) || !bytes.Equal(x.Payload, []byte(payload)) {
		c.t.Fatalf("got %s, want %s seq %d ack %d %q", segStr(x), flagStr(flags), c.ack, c.seq, payload)
	}
	c.ack += uint32(len(x.Payload))
	if x.Flags&(sim.FIN|sim.SYN) != 0 {
		c.ack++
	}
	return x
}

// accept takes the VM's SYN (active open): checks its MSS and window and
// learns its port and sequence number.
func (c *tcpConn) accept(s []sim.TCP) sim.TCP {
	c.t.Helper()
	if len(s) != 1 || s[0].Flags != sim.SYN || s[0].DstPort != c.pport {
		c.t.Fatalf("want a SYN to port %d, got %s", c.pport, segsStr(s))
	}
	syn := s[0]
	if m, ok := syn.MSS(); !ok || m != 1460 || syn.Window != 1460 {
		c.t.Fatalf("SYN: MSS %d %v window %d", m, ok, syn.Window)
	}
	c.vport = syn.SrcPort
	c.ack = syn.Seq + 1
	return syn
}

func flagStr(f uint8) string {
	s := ""
	for i, n := range []string{"F", "S", "R", "P", "A"} {
		if f&(1<<i) != 0 {
			s += n
		}
	}
	return "[" + s + "]"
}

func segStr(x sim.TCP) string {
	return flagStr(x.Flags) + " " + strconv.Itoa(int(x.SrcPort)) + ">" + strconv.Itoa(int(x.DstPort)) + " seq " +
		strconv.Itoa(int(x.Seq)) + " ack " + strconv.Itoa(int(x.Ack)) + " win " + strconv.Itoa(int(x.Window)) + " len " + strconv.Itoa(len(x.Payload))
}

func segsStr(s []sim.TCP) string {
	out := ""
	for _, x := range s {
		out += "\n  " + segStr(x)
	}
	return out
}

// startTCP starts progs/<name> (it sets 10.0.0.2/24, gateway 10.0.0.1)
// and answers its ARP request for the gateway.
func startTCP(t *testing.T, name string) *sim.VM {
	t.Helper()
	v := start(t, name)
	arpFor(t, one(t)(v.Sync()), vmHost.IP, gwHost.IP)
	none(t)(v.Exchange(arpReply(gwHost, vmHost)))
	return v
}
