package nettest

import (
	"bytes"
	"os"
	"path/filepath"
	"strconv"
	"testing"

	"gogo/tests/net/sim"
)

// The DHCP server is the gateway; it offers the VM 10.0.0.2/24 with DNS
// 10.0.0.53.
var (
	dnsIP  = sim.IPv4(10, 0, 0, 53)
	mask24 = sim.IPv4(255, 255, 255, 0)
	bcast  = sim.Host{MAC: sim.Broadcast, IP: bcastIP}
)

// dhcpFrom checks p is a DHCP request of type typ from the VM's client
// port to the server port (broadcast from 0.0.0.0 unless ciaddr is set)
// and returns it.
func dhcpFrom(t *testing.T, p sim.Packet, typ uint8) sim.DHCP {
	t.Helper()
	if p.UDP == nil || p.UDP.SrcPort != sim.DHCPClientPort || p.UDP.DstPort != sim.DHCPServerPort {
		t.Fatalf("want DHCP from the VM, got %+v %+v", p.IP, p.UDP)
	}
	d, err := sim.ParseDHCP(p.UDP.Payload)
	if err != nil {
		t.Fatalf("DHCP: %v", err)
	}
	if d.Op != 1 || d.CHAddr != sim.VMMAC || d.MsgType() != typ {
		t.Fatalf("want DHCP type %d with chaddr %v, got op %d chaddr %v type %d", typ, sim.VMMAC, d.Op, d.CHAddr, d.MsgType())
	}
	if pl, _ := d.Opt(sim.OptParamList); !bytes.Equal(pl, []byte{1, 3, 6}) {
		t.Fatalf("parameter list % x", pl)
	}
	if d.CIAddr == anyIP {
		if p.Eth.Dst != sim.Broadcast || p.IP.Src != anyIP || p.IP.Dst != bcastIP || d.Flags != 0x8000 {
			t.Fatalf("want a broadcast from 0.0.0.0 with the broadcast flag: %+v %+v flags %#x", p.Eth, p.IP, d.Flags)
		}
	}
	return d
}

// leaseReply is the server's reply of type typ to req for vmHost.
func leaseReply(req sim.DHCP, typ uint8, leaseS uint32) sim.DHCP {
	return sim.DHCPReply(req, typ, gwHost.IP, vmHost.IP, mask24, gwHost.IP, dnsIP, leaseS)
}

// serve sends reply to the VM (broadcast, as the flag asks; unicast when
// the VM has an address) and returns the frames it sent in response.
func serve(v *sim.VM, reply sim.DHCP, unicast bool) ([][]byte, error) {
	to := bcast
	if unicast {
		to = vmHost
	}
	return v.Exchange(sim.UDPFrame(gwHost, to, sim.UDP{SrcPort: 67, DstPort: 68, Payload: reply.Bytes()}))
}

// lease runs DISCOVER -> OFFER -> REQUEST -> ACK with a leaseS-second
// lease, answers the gateway ARP, and returns the ACK time.
func lease(t *testing.T, v *sim.VM, leaseS uint32) (sim.DHCP, uint64) {
	t.Helper()
	return leaseFrom(t, v, dhcpFrom(t, one(t)(v.Sync()), sim.DHCPDiscover), leaseS)
}

// leaseFrom is lease after the DISCOVER disc.
func leaseFrom(t *testing.T, v *sim.VM, disc sim.DHCP, leaseS uint32) (sim.DHCP, uint64) {
	t.Helper()
	req := dhcpFrom(t, one(t)(serve(v, leaseReply(disc, sim.DHCPOffer, leaseS), false)), sim.DHCPRequest)
	if req.XID != disc.XID {
		t.Fatalf("REQUEST xid %#x, DISCOVER %#x", req.XID, disc.XID)
	}
	if ip, _ := req.OptIP(sim.OptRequestedIP); ip != vmHost.IP {
		t.Fatalf("requested IP %v", ip)
	}
	if ip, _ := req.OptIP(sim.OptServerID); ip != gwHost.IP {
		t.Fatalf("server id %v", ip)
	}
	arpFor(t, one(t)(serve(v, leaseReply(req, sim.DHCPAck, leaseS), false)), vmHost.IP, gwHost.IP)
	none(t)(v.Exchange(arpReply(gwHost, vmHost)))
	return req, v.Now()
}

// stop closes the link (the VM exits 0) and checks its output.
func stop(t *testing.T, v *sim.VM, want string) {
	t.Helper()
	if err := v.Wait(); err != nil {
		t.Errorf("VM: %v", err)
	}
	if got := v.Stdout.String(); got != want {
		t.Errorf("stdout\n--- got ---\n%s--- want ---\n%s", got, want)
	}
}

func itoa(n uint64) string { return strconv.FormatUint(n, 10) }

var upLine = "up " + ipInt(vmHost.IP) + " " + ipInt(gwHost.IP) + " " + ipInt(dnsIP)

// DISCOVER is retransmitted with backoff (2, 4, 8 s); a reply for another
// transaction is ignored; the lease sets the address, Start returns true
// and subscribers get Up.
func TestDHCPLease(t *testing.T) {
	v := start(t, "dhcp_client")
	disc := dhcpFrom(t, one(t)(v.Sync()), sim.DHCPDiscover)
	for _, wait := range []uint32{2000, 4000, 8000} {
		none(t)(v.Advance(wait - 1))
		d := dhcpFrom(t, one(t)(v.Advance(1)), sim.DHCPDiscover)
		if d.XID != disc.XID {
			t.Fatalf("retransmission xid %#x, want %#x", d.XID, disc.XID)
		}
	}
	other := leaseReply(disc, sim.DHCPOffer, 3600)
	other.XID++
	none(t)(serve(v, other, false))
	noMagic := leaseReply(disc, sim.DHCPOffer, 3600).Bytes()
	noMagic[236] = 0
	none(t)(v.Exchange(sim.UDPFrame(gwHost, bcast, sim.UDP{SrcPort: 67, DstPort: 68, Payload: noMagic})))
	req := dhcpFrom(t, one(t)(serve(v, leaseReply(disc, sim.DHCPOffer, 3600), false)), sim.DHCPRequest)
	// REQUEST is retransmitted too.
	none(t)(v.Advance(1999))
	dhcpFrom(t, one(t)(v.Advance(1)), sim.DHCPRequest)
	// A second offer while requesting is ignored, and so are ACKs from
	// another server or for another address.
	none(t)(serve(v, leaseReply(disc, sim.DHCPOffer, 3600), false))
	otherSrv := sim.DHCPReply(req, sim.DHCPAck, sim.IPv4(10, 0, 0, 254), vmHost.IP, mask24, gwHost.IP, dnsIP, 3600)
	none(t)(serve(v, otherSrv, false))
	otherIP := sim.DHCPReply(req, sim.DHCPAck, gwHost.IP, sim.IPv4(10, 0, 0, 77), mask24, gwHost.IP, dnsIP, 3600)
	none(t)(serve(v, otherIP, false))
	arpFor(t, one(t)(serve(v, leaseReply(req, sim.DHCPAck, 3600), false)), vmHost.IP, gwHost.IP)
	at := v.Now()
	// Bound: nothing until T1 (30 min).
	none(t)(v.Advance(1799999))
	stop(t, v, "subscribe true\nstart true "+itoa(at)+"\n"+upLine+" "+itoa(at)+"\n")
}

// At T1 the client renews by unicast to the server; at T2 it rebinds by
// broadcast; each ACK extends the lease with no Down.
func TestDHCPRenew(t *testing.T) {
	v := start(t, "dhcp_client")
	_, at := lease(t, v, 20) // T1 10 s, T2 17.5 s
	none(t)(v.Advance(9999))
	p := one(t)(v.Advance(1))
	r := dhcpFrom(t, p, sim.DHCPRequest)
	if p.Eth.Dst != gwHost.MAC || p.IP.Src != vmHost.IP || p.IP.Dst != gwHost.IP || r.CIAddr != vmHost.IP || r.Flags != 0 {
		t.Fatalf("renew: want unicast from the leased address: %+v %+v %+v", p.Eth, p.IP, r)
	}
	if _, ok := r.Opt(sim.OptRequestedIP); ok {
		t.Fatal("renew carries a requested IP")
	}
	if _, ok := r.Opt(sim.OptServerID); ok {
		t.Fatal("renew carries a server id")
	}
	none(t)(serve(v, leaseReply(r, sim.DHCPAck, 20), true)) // renewed at 10 s: T1 20 s
	none(t)(v.Advance(9999))
	dhcpFrom(t, one(t)(v.Advance(1)), sim.DHCPRequest) // T1 (at 20 s); unanswered
	// Renew retries at half the time left to T2 (27.5 s): 23.75, 25.625, 26.625, then T2.
	for _, wait := range []uint32{3750, 1875, 1000} {
		none(t)(v.Advance(wait - 1))
		dhcpFrom(t, one(t)(v.Advance(1)), sim.DHCPRequest)
	}
	none(t)(v.Advance(874))
	p = one(t)(v.Advance(1))
	r = dhcpFrom(t, p, sim.DHCPRequest)
	if p.Eth.Dst != sim.Broadcast || p.IP.Src != vmHost.IP || p.IP.Dst != bcastIP || r.CIAddr != vmHost.IP {
		t.Fatalf("rebind: want a broadcast from the leased address: %+v %+v %+v", p.Eth, p.IP, r)
	}
	none(t)(serve(v, leaseReply(r, sim.DHCPAck, 20), false))
	none(t)(v.Advance(9999))
	dhcpFrom(t, one(t)(v.Advance(1)), sim.DHCPRequest) // the new T1
	stop(t, v, "subscribe true\nstart true "+itoa(at)+"\n"+upLine+" "+itoa(at)+"\n")
}

// A lease that cannot be renewed or rebound expires: the address is
// cleared (subscribers get Down) and DISCOVER starts over. A NAK to a
// renewal does the same at once.
func TestDHCPLost(t *testing.T) {
	v := start(t, "dhcp_client")
	_, at := lease(t, v, 20)
	none(t)(v.Advance(9999))
	n := 0
	for v.Now() < at+20000 {
		frames, err := v.Advance(250)
		if err != nil {
			t.Fatal(err)
		}
		for _, f := range frames {
			if v.Now() < at+20000 {
				dhcpFrom(t, decode(t, f), sim.DHCPRequest)
				n++
			} else {
				dhcpFrom(t, decode(t, f), sim.DHCPDiscover) // expired: 0.0.0.0 again
				n = -n
			}
		}
	}
	if n != -7 { // renew at 10, 13.75, 15.625, 16.625 s; rebind at 17.5, 18.75, 19.75 s; then DISCOVER
		t.Fatalf("%d requests before DISCOVER, want 7", -n)
	}
	down := at + 20000
	// Lease again; the renewal is NAKed: Down and DISCOVER at once.
	none(t)(v.Advance(1000))
	disc := dhcpFrom(t, one(t)(v.Advance(1000)), sim.DHCPDiscover) // the 2 s retransmission
	_, at2 := leaseFrom(t, v, disc, 20)
	none(t)(v.Advance(9999))
	r := dhcpFrom(t, one(t)(v.Advance(1)), sim.DHCPRequest)
	dhcpFrom(t, one(t)(serve(v, leaseReply(r, sim.DHCPNak, 0), true)), sim.DHCPDiscover)
	nak := v.Now()
	stop(t, v, "subscribe true\nstart true "+itoa(at)+"\n"+upLine+" "+itoa(at)+"\ndown "+itoa(down)+"\n"+
		upLine+" "+itoa(at2)+"\ndown "+itoa(nak)+"\n")
}

// With net.ip in the configuration store DHCP is skipped.
func TestDHCPStatic(t *testing.T) {
	cfg := filepath.Join(t.TempDir(), "config")
	if err := os.WriteFile(cfg, []byte("net.ip=10.0.0.2\nnet.gw=10.0.0.1\nnet.dns=10.0.0.53\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	t.Setenv("GOGO_CONFIG", cfg)
	v := start(t, "dhcp_client")
	arpFor(t, one(t)(v.Sync()), vmHost.IP, gwHost.IP)
	none(t)(v.Advance(60000))
	stop(t, v, "subscribe true\nstart true 0\n"+upLine+" 0\n")
}

// Start gives up after TimeoutMs; the client carries on and a later lease
// still brings Up.
func TestDHCPStartTimeout(t *testing.T) {
	cfg := filepath.Join(t.TempDir(), "config")
	if err := os.WriteFile(cfg, []byte("test.timeout=5000\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	t.Setenv("GOGO_CONFIG", cfg)
	v := start(t, "dhcp_client")
	dhcpFrom(t, one(t)(v.Sync()), sim.DHCPDiscover)
	dhcpFrom(t, one(t)(v.Advance(2000)), sim.DHCPDiscover)
	none(t)(v.Advance(3000)) // Start returns false at 5 s
	disc := dhcpFrom(t, one(t)(v.Advance(1000)), sim.DHCPDiscover)
	_, at := leaseFrom(t, v, disc, 3600)
	stop(t, v, "subscribe true\nstart false 5000\n"+upLine+" "+itoa(at)+"\n")
}

// A renewal ACK may move the client to another address: the stack takes
// it (subscribers get Up with it) and later renewals come from it.
func TestDHCPRenewNewAddr(t *testing.T) {
	v := start(t, "dhcp_client")
	_, at := lease(t, v, 20)
	none(t)(v.Advance(9999))
	r := dhcpFrom(t, one(t)(v.Advance(1)), sim.DHCPRequest)
	moved := sim.Host{MAC: sim.VMMAC, IP: sim.IPv4(10, 0, 0, 77)}
	ack := sim.DHCPReply(r, sim.DHCPAck, gwHost.IP, moved.IP, mask24, gwHost.IP, dnsIP, 20)
	arpFor(t, one(t)(serve(v, ack, true)), moved.IP, gwHost.IP) // SetAddr: the gateway again, from .77
	moveAt := v.Now()
	none(t)(v.Exchange(arpReply(gwHost, moved)))
	none(t)(v.Advance(9999))
	p := one(t)(v.Advance(1))
	r = dhcpFrom(t, p, sim.DHCPRequest)
	if p.IP.Src != moved.IP || r.CIAddr != moved.IP {
		t.Fatalf("renew after the move: from %v ciaddr %v, want %v", p.IP.Src, r.CIAddr, moved.IP)
	}
	stop(t, v, "subscribe true\nstart true "+itoa(at)+"\n"+upLine+" "+itoa(at)+"\nup "+ipInt(moved.IP)+" "+
		ipInt(gwHost.IP)+" "+ipInt(dnsIP)+" "+itoa(moveAt)+"\n")
}
