package nettest

import (
	"bytes"
	"io"
	"strconv"
	"strings"
	"testing"

	"gogotools/tests/net/sim"
)

// The VM is 10.0.0.2/24; the peer is its gateway 10.0.0.1.
var (
	vmHost   = sim.Host{MAC: sim.VMMAC, IP: sim.IPv4(10, 0, 0, 2)}
	gwHost   = sim.Host{MAC: sim.MAC{2, 0, 0, 0, 0, 0x99}, IP: sim.IPv4(10, 0, 0, 1)}
	bcastIP  = sim.IPv4(255, 255, 255, 255)
	anyIP    sim.IP
	farIP    = sim.IPv4(100, 64, 0, 9) // off-subnet: through the gateway
	otherMAC = sim.MAC{2, 0, 0, 0, 0, 0x55}
)

func ipInt(ip sim.IP) string { return strconv.Itoa(int(int32(ip.Int()))) }

// decode parses frame, failing the test on any length or checksum error.
func decode(t *testing.T, frame []byte) sim.Packet {
	t.Helper()
	p, err := sim.Decode(frame)
	if err != nil {
		t.Fatalf("bad frame from the VM: %v\n% x", err, frame)
	}
	return p
}

// one expects exactly one frame: one(t)(v.Sync()).
func one(t *testing.T) func([][]byte, error) sim.Packet {
	return func(frames [][]byte, err error) sim.Packet {
		t.Helper()
		if err != nil {
			t.Fatal(err)
		}
		if len(frames) != 1 {
			t.Fatalf("%d frames, want 1", len(frames))
		}
		return decode(t, frames[0])
	}
}

// none expects no frame: none(t)(v.Sync()).
func none(t *testing.T) func([][]byte, error) {
	return func(frames [][]byte, err error) {
		t.Helper()
		if err != nil {
			t.Fatal(err)
		}
		if len(frames) != 0 {
			t.Fatalf("%d frames, want none: first % x", len(frames), frames[0])
		}
	}
}

// arpFor checks p is an ARP request for ip from the VM.
func arpFor(t *testing.T, p sim.Packet, from, ip sim.IP) {
	t.Helper()
	if p.ARP == nil || p.ARP.Op != sim.ARPRequest || p.Eth.Dst != sim.Broadcast || p.ARP.SHA != sim.VMMAC ||
		p.ARP.SPA != from || p.ARP.TPA != ip {
		t.Fatalf("want an ARP request for %v from %v, got %+v %+v", ip, from, p.Eth, p.ARP)
	}
}

func arpRequest(from sim.Host, target sim.IP) []byte {
	return sim.ARPFrame(sim.ARP{Op: sim.ARPRequest, SHA: from.MAC, SPA: from.IP, TPA: target})
}

func arpReply(from, to sim.Host) []byte {
	return sim.ARPFrame(sim.ARP{Op: sim.ARPReply, SHA: from.MAC, SPA: from.IP, THA: to.MAC, TPA: to.IP})
}

// The stack answers ARP requests for its address once it has one, and
// pre-resolves the gateway when the address is set; subscribers get Up.
func TestARPReply(t *testing.T) {
	v := start(t, "arp_reply")
	none(t)(v.Sync())
	none(t)(v.Exchange(arpRequest(gwHost, vmHost.IP))) // no address yet
	arpFor(t, one(t)(v.Advance(1000)), vmHost.IP, gwHost.IP)
	p := one(t)(v.Exchange(arpRequest(gwHost, vmHost.IP)))
	if p.ARP == nil || p.ARP.Op != sim.ARPReply || p.Eth.Dst != gwHost.MAC || p.Eth.Src != sim.VMMAC ||
		p.ARP.SHA != sim.VMMAC || p.ARP.SPA != vmHost.IP || p.ARP.THA != gwHost.MAC || p.ARP.TPA != gwHost.IP {
		t.Fatalf("ARP reply %+v %+v", p.Eth, p.ARP)
	}
	none(t)(v.Exchange(arpRequest(gwHost, sim.IPv4(10, 0, 0, 9))))                              // another host
	none(t)(v.Exchange(arpReply(gwHost, vmHost)))                                               // a reply: learned, not answered
	none(t)(v.Exchange(sim.Pad(sim.Eth{Dst: sim.Broadcast, Src: gwHost.MAC, Type: sim.EtherARP, // short
		Payload: []byte{0, 1, 8, 0, 6, 4, 0, 1}}.Bytes())))
	if _, err := v.Advance(5000); err != io.EOF {
		t.Fatalf("advance: %v, want EOF", err)
	}
	finish(t, v, "subscribe true\nnetif true\nup "+ipInt(vmHost.IP)+" "+ipInt(gwHost.IP)+" "+
		ipInt(sim.IPv4(10, 0, 0, 53))+"\ndone\n")
}

// Echo requests to the VM's address are answered to the sender's MAC,
// payload intact; others are ignored.
func TestPing(t *testing.T) {
	v := start(t, "ping")
	arpFor(t, one(t)(v.Sync()), vmHost.IP, gwHost.IP)
	data := []byte("0123456789abcdefghijklmnopqrstuvwxyz")
	for _, from := range []sim.Host{gwHost, {MAC: gwHost.MAC, IP: farIP}, {MAC: otherMAC, IP: sim.IPv4(10, 0, 0, 7)}} {
		req := sim.ICMP{Type: sim.ICMPEchoRequest, ID: 0x4242, Seq: 3, Data: data}
		p := one(t)(v.Exchange(sim.ICMPFrame(from, vmHost, req)))
		if p.ICMP == nil || p.ICMP.Type != sim.ICMPEchoReply || p.ICMP.ID != 0x4242 || p.ICMP.Seq != 3 ||
			!bytes.Equal(p.ICMP.Data, data) || p.Eth.Dst != from.MAC || p.IP.Src != vmHost.IP || p.IP.Dst != from.IP {
			t.Fatalf("echo reply to %v: %+v %+v %+v", from.IP, p.Eth, p.IP, p.ICMP)
		}
	}
	// To another address, to the broadcast address, a bad checksum: nothing.
	none(t)(v.Exchange(sim.ICMPFrame(gwHost, sim.Host{MAC: sim.VMMAC, IP: sim.IPv4(10, 0, 0, 3)},
		sim.ICMP{Type: sim.ICMPEchoRequest, Data: data})))
	none(t)(v.Exchange(sim.ICMPFrame(gwHost, sim.Host{MAC: sim.Broadcast, IP: bcastIP},
		sim.ICMP{Type: sim.ICMPEchoRequest, Data: data})))
	bad := sim.ICMPFrame(gwHost, vmHost, sim.ICMP{Type: sim.ICMPEchoRequest, Data: data})
	bad[len(bad)-1] ^= 0xFF
	none(t)(v.Exchange(bad))
	if _, err := v.Advance(10000); err != io.EOF {
		t.Fatalf("advance: %v, want EOF", err)
	}
	finish(t, v, "done\n")
}

// UDP echo: datagrams from the gateway, from beyond it and from an
// on-link host learned from its packet are echoed back, the largest
// payload included.
func TestUDPEcho(t *testing.T) {
	v := start(t, "udp_echo")
	arpFor(t, one(t)(v.Sync()), vmHost.IP, gwHost.IP)
	none(t)(v.Exchange(arpReply(gwHost, vmHost)))
	big := bytes.Repeat([]byte("gogo"), 368) // 1472 bytes: a full 1500-byte packet
	cases := []struct {
		from    sim.Host
		port    uint16
		payload []byte
	}{
		{gwHost, 5000, []byte("hello")},
		{sim.Host{MAC: gwHost.MAC, IP: farIP}, 53, []byte("far away")},
		{sim.Host{MAC: otherMAC, IP: sim.IPv4(10, 0, 0, 7)}, 7777, nil},
		{gwHost, 5001, big},
	}
	var want strings.Builder
	want.WriteString("udp true\n")
	for i, c := range cases {
		f := sim.UDPFrame(c.from, vmHost, sim.UDP{SrcPort: c.port, DstPort: 7, Payload: c.payload})
		frames, err := v.Exchange(f)
		if err == io.EOF && i == len(cases)-1 {
			err = nil // the VM exits after its last echo
		}
		p := one(t)(frames, err)
		if p.UDP == nil || p.UDP.SrcPort != 7 || p.UDP.DstPort != c.port || !bytes.Equal(p.UDP.Payload, c.payload) ||
			p.Eth.Dst != c.from.MAC || p.IP.Src != vmHost.IP || p.IP.Dst != c.from.IP || p.UDP.NoChecksum {
			t.Fatalf("echo to %v:%d: %+v %+v %+v", c.from.IP, c.port, p.Eth, p.IP, p.UDP)
		}
		want.WriteString("echo " + ipInt(c.from.IP) + " " + strconv.Itoa(int(c.port)) + " " + strconv.Itoa(len(c.payload)) + "\n")
	}
	if _, err := v.Sync(); err != io.EOF {
		t.Fatalf("sync: %v, want EOF", err)
	}
	want.WriteString("done\n")
	finish(t, v, want.String())
}

// A reader over its quota loses datagrams but is never killed, and
// receives again once it has room.
func TestUDPQuota(t *testing.T) {
	v := start(t, "udp_quota")
	arpFor(t, one(t)(v.Sync()), vmHost.IP, gwHost.IP)
	payload := bytes.Repeat([]byte{0xA5}, 500)
	const sent = 12
	for i := 0; i < sent; i++ {
		none(t)(v.Exchange(sim.UDPFrame(gwHost, vmHost, sim.UDP{SrcPort: 9000, DstPort: 9, Payload: payload})))
	}
	none(t)(v.Advance(1000))
	// The reader prints, tells main, and the VM exits.
	if _, err := v.Exchange(sim.UDPFrame(gwHost, vmHost, sim.UDP{SrcPort: 9000, DstPort: 9, Payload: payload[:33]})); err != io.EOF {
		t.Fatalf("exchange: %v, want EOF", err)
	}
	// The link closed, but the last lines may still be in the stdout pipe:
	// read the output once the VM has exited.
	if !v.Closed() {
		t.Error("VM still running (link open)")
	}
	if err := v.Wait(); err != nil {
		t.Errorf("VM: %v (want exit status 0)", err)
	}
	out := v.Stdout.String()
	lines := strings.Split(out, "\n")
	if len(lines) != 5 || lines[0] != "udp true" || lines[2] != "after 33" || lines[3] != "done" {
		t.Fatalf("stdout %q", out)
	}
	got, err := strconv.Atoi(strings.TrimPrefix(lines[1], "got "))
	if err != nil || got < 1 || got >= sent {
		t.Fatalf("%q: want some but not all of %d datagrams", lines[1], sent)
	}
	t.Logf("delivered %d of %d", got, sent)
}

// Without an address only DHCP traffic passes, replies only for our MAC.
func TestUDPUnconfigured(t *testing.T) {
	v := start(t, "udp_unconfigured")
	p := one(t)(v.Sync()) // only the broadcast to port 67 goes out
	if p.UDP == nil || p.Eth.Dst != sim.Broadcast || p.IP.Src != anyIP || p.IP.Dst != bcastIP ||
		p.UDP.SrcPort != 68 || p.UDP.DstPort != 67 || string(p.UDP.Payload) != "discover" {
		t.Fatalf("discover: %+v %+v %+v", p.Eth, p.IP, p.UDP)
	}
	none(t)(v.Exchange(sim.UDPFrame(gwHost, vmHost, sim.UDP{SrcPort: 9, DstPort: 7, Payload: []byte("x")})))
	none(t)(v.Exchange(sim.UDPFrame(gwHost, vmHost, sim.UDP{SrcPort: 66, DstPort: 68, Payload: []byte("x")})))
	none(t)(v.Exchange(arpRequest(gwHost, anyIP)))
	bcast := sim.Host{MAC: sim.Broadcast, IP: bcastIP}
	reply := sim.DHCP{Op: 2, XID: 7, CHAddr: otherMAC, Options: []sim.DHCPOpt{{Code: sim.OptMsgType, Data: []byte{sim.DHCPOffer}}}}
	// Another client's offer, and one too short to name a client: filtered.
	none(t)(v.Exchange(sim.UDPFrame(gwHost, bcast, sim.UDP{SrcPort: 67, DstPort: 68, Payload: reply.Bytes()})))
	none(t)(v.Exchange(sim.UDPFrame(gwHost, bcast, sim.UDP{SrcPort: 67, DstPort: 68, Payload: []byte("offer")})))
	reply.CHAddr = sim.VMMAC
	offer := reply.Bytes()
	if _, err := v.Exchange(sim.UDPFrame(gwHost, bcast, sim.UDP{SrcPort: 67, DstPort: 68, Payload: offer})); err != io.EOF {
		t.Fatalf("exchange: %v, want EOF", err)
	}
	finish(t, v, "got "+ipInt(gwHost.IP)+" 67 "+strconv.Itoa(len(offer))+" 2\n")
}
