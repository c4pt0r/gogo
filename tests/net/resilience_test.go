package nettest

import (
	"testing"

	"gogotools/tests/net/sim"
)

// connect leases an address for leaseS seconds and brings up the
// program's TCP connection to the gateway's port 80 (the program dials
// 100 ms after Up).
func connect(t *testing.T, v *sim.VM, leaseS uint32) *tcpConn {
	t.Helper()
	lease(t, v, leaseS)
	return redial(t, v)
}

// redial completes the program's next dial, 100 ms from now.
func redial(t *testing.T, v *sim.VM) *tcpConn {
	t.Helper()
	c := newConn(t, v, 80, 0)
	c.accept(c.segs(v.Advance(100)))
	c.expect(c.send(sim.SYN|sim.ACK, nil), sim.ACK, "")
	return c
}

// The link drops: subscribers get Down and the connection ends with
// NoRoute (no RST on the dead link), the peer's later segments for it get
// a RST. The link returns: Up again, the gateway is re-resolved, the
// DHCP client renews at once (its sends failed while the link was down),
// the unchanged lease raises no second Up, and the program redials.
func TestLinkFlap(t *testing.T) {
	v := start(t, "resilient")
	c := connect(t, v, 3600)
	t0 := v.Now() // 100
	none(t)(v.SetLink(false))
	none(t)(v.Advance(500)) // nothing leaves a dead link
	fr, err := v.SetLink(true)
	if err != nil || len(fr) != 2 {
		t.Fatalf("link up: %d frames, %v; want gateway ARP + DHCP renewal", len(fr), err)
	}
	arpFor(t, decode(t, fr[0]), vmHost.IP, gwHost.IP)
	p := decode(t, fr[1])
	r := dhcpFrom(t, p, sim.DHCPRequest)
	if p.Eth.Dst != gwHost.MAC || p.IP.Dst != gwHost.IP || r.CIAddr != vmHost.IP {
		t.Fatalf("renewal: %+v %+v %+v", p.Eth, p.IP, r)
	}
	none(t)(v.Exchange(arpReply(gwHost, vmHost)))
	none(t)(serve(v, leaseReply(r, sim.DHCPAck, 3600), true)) // unchanged: no new Up
	// The old connection is gone: its next segment is answered with RST.
	s := c.send(sim.PSH|sim.ACK, []byte("late"))
	if len(s) != 1 || s[0].Flags&sim.RST == 0 {
		t.Fatalf("segment for the dead connection: %s", segsStr(s))
	}
	c2 := newConn(t, v, 80, 0)
	c2.accept(c2.segs(v.Advance(100)))
	c2.expect(c2.send(sim.SYN|sim.ACK, nil), sim.ACK, "")
	if c2.vport == c.vport {
		t.Errorf("redial reused port %d", c.vport)
	}
	up := t0 + 500 // when the link returned
	stop(t, v, "subscribe true\nstart true 0\nup 0\ndial true\nconnected "+itoa(t0)+"\n"+
		"closed 20 "+itoa(t0)+"\ndown "+itoa(t0)+"\nget false "+itoa(t0)+"\nup "+itoa(up)+"\ndial true\nconnected "+itoa(up+100)+"\n")
}

// The lease runs out unrenewed: SetAddr zero gives Down, the connection
// ends with NoRoute (later segments are dropped: no address), the client starts over with DISCOVER; a new lease
// brings Up and the program redials.
func TestLeaseLossClosesConns(t *testing.T) {
	v := start(t, "resilient")
	c := connect(t, v, 20) // T1 10 s, T2 17.5 s, expiry 20 s
	fr, err := v.Advance(20000 - 1 - uint32(v.Now()))
	if err != nil || len(fr) < 4 {
		t.Fatalf("%d frames before expiry, %v; want the unanswered renewals", len(fr), err)
	}
	for _, f := range fr {
		dhcpFrom(t, decode(t, f), sim.DHCPRequest)
	}
	disc := dhcpFrom(t, one(t)(v.Advance(1)), sim.DHCPDiscover)
	none(t)(v.Exchange(c.frame(sim.PSH|sim.ACK, []byte("late")))) // no address: dropped
	at := v.Now()
	leaseFrom(t, v, disc, 3600)
	redial(t, v)
	stop(t, v, "subscribe true\nstart true 0\nup 0\ndial true\nconnected 100\n"+
		"closed 20 20000\ndown 20000\nget false 20000\nup "+itoa(at)+"\ndial true\nconnected "+itoa(at+100)+"\n")
}

// The lease runs out while the link is down: after the link returns and a
// new lease arrives, the client does not renew at once (the Down that
// preceded the expiry no longer counts).
func TestLeaseExpiresDuringOutage(t *testing.T) {
	v := start(t, "resilient")
	connect(t, v, 20)
	none(t)(v.SetLink(false))
	none(t)(v.Advance(20000)) // renewals and the expiry all fail on the dead link
	none(t)(v.SetLink(true))
	fr, err := v.Advance(4000) // DISCOVER retransmissions resume
	if err != nil || len(fr) == 0 {
		t.Fatalf("%d frames, %v; want a DISCOVER", len(fr), err)
	}
	disc := dhcpFrom(t, decode(t, fr[len(fr)-1]), sim.DHCPDiscover)
	leaseFrom(t, v, disc, 3600)
	redial(t, v)             // the program's dial 100 ms after Up; a stale flag would send a renewal here
	none(t)(v.Advance(1000)) // no spurious renewal after the ACK
	none(t)(v.Advance(1000000))
	v.Wait()
}
