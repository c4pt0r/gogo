package sim

import (
	"bytes"
	"encoding/binary"
	"io"
	"net"
	"testing"
	"time"
)

var (
	peer = Host{MAC{0x02, 0, 0, 0, 0, 0x99}, IPv4(10, 0, 0, 1)}
	vm   = Host{VMMAC, IPv4(10, 0, 0, 2)}
)

// RFC 1071-style vector: a real IPv4 header whose checksum is 0xb861.
func TestChecksum(t *testing.T) {
	h := []byte{0x45, 0, 0, 0x73, 0, 0, 0x40, 0, 0x40, 0x11, 0, 0, 0xc0, 0xa8, 0, 1, 0xc0, 0xa8, 0, 0xc7}
	if c := Checksum(h); c != 0xb861 {
		t.Fatalf("checksum %#x, want 0xb861", c)
	}
	binary.BigEndian.PutUint16(h[10:], 0xb861)
	if Checksum(h) != 0 {
		t.Fatal("header with its checksum does not sum to 0")
	}
	if Checksum([]byte{1}) != ^uint16(0x0100) {
		t.Fatal("odd length")
	}
}

func TestIPInt(t *testing.T) {
	ip := IPv4(192, 168, 1, 20)
	if ip.Int() != 0xC0A80114 || IPFromInt(0xC0A80114) != ip || ip.String() != "192.168.1.20" {
		t.Fatal(ip.Int(), ip)
	}
}

func TestARPRoundTrip(t *testing.T) {
	f := ARPFrame(ARP{Op: ARPRequest, SHA: peer.MAC, SPA: peer.IP, TPA: vm.IP})
	if len(f) != 60 {
		t.Fatalf("len %d, want padded 60", len(f))
	}
	p, err := Decode(f)
	if err != nil || p.ARP == nil {
		t.Fatal(err)
	}
	if p.Eth.Dst != Broadcast || p.ARP.Op != ARPRequest || p.ARP.TPA != vm.IP || p.ARP.SHA != peer.MAC {
		t.Fatalf("%+v", p.ARP)
	}
}

func TestUDPRoundTrip(t *testing.T) {
	f := UDPFrame(peer, vm, UDP{SrcPort: 5000, DstPort: 9000, Payload: []byte("hello")})
	p, err := Decode(f)
	if err != nil || p.UDP == nil {
		t.Fatal(err)
	}
	if p.IP.Src != peer.IP || p.IP.Dst != vm.IP || p.IP.TTL != 64 || p.UDP.SrcPort != 5000 ||
		p.UDP.DstPort != 9000 || string(p.UDP.Payload) != "hello" {
		t.Fatalf("%+v %+v", p.IP, p.UDP)
	}
	// Padding after the IP total length is dropped.
	if len(p.IP.Payload) != 8+5 {
		t.Fatalf("ip payload %d", len(p.IP.Payload))
	}
	// Corruptions are caught.
	bad := append([]byte(nil), f...)
	bad[14+20+8] ^= 1 // payload byte: UDP checksum
	if _, err := Decode(bad); err == nil {
		t.Fatal("corrupt UDP payload accepted")
	}
	bad = append([]byte(nil), f...)
	bad[14+8] ^= 1 // TTL: IP header checksum
	if _, err := Decode(bad); err == nil {
		t.Fatal("corrupt IP header accepted")
	}
	if _, err := Decode(f[:20]); err == nil {
		t.Fatal("short frame accepted")
	}
	// No checksum is accepted.
	p, err = Decode(UDPFrame(peer, vm, UDP{SrcPort: 1, DstPort: 2, Payload: []byte{7}, NoChecksum: true}))
	if err != nil || !p.UDP.NoChecksum {
		t.Fatal(err)
	}
}

func TestTCPRoundTrip(t *testing.T) {
	seg := TCP{SrcPort: 40000, DstPort: 80, Seq: 1000, Ack: 2000, Flags: SYN | ACK, Window: 1460,
		Options: MSSOption(1460), Payload: []byte("GET /")}
	p, err := Decode(TCPFrame(peer, vm, seg))
	if err != nil || p.TCP == nil {
		t.Fatal(err)
	}
	g := p.TCP
	if g.Seq != 1000 || g.Ack != 2000 || g.Flags != SYN|ACK || g.Window != 1460 || string(g.Payload) != "GET /" {
		t.Fatalf("%+v", g)
	}
	if mss, ok := g.MSS(); !ok || mss != 1460 {
		t.Fatal("mss", mss, ok)
	}
	if _, ok := (TCP{Options: []byte{1, 1, 0}}).MSS(); ok {
		t.Fatal("mss from nops")
	}
	f := TCPFrame(peer, vm, seg)
	f[14+20+4] ^= 0x80
	if _, err := Decode(f); err == nil {
		t.Fatal("corrupt TCP accepted")
	}
}

func TestICMPRoundTrip(t *testing.T) {
	p, err := Decode(ICMPFrame(peer, vm, ICMP{Type: ICMPEchoRequest, ID: 7, Seq: 3, Data: []byte("ping")}))
	if err != nil || p.ICMP == nil || p.ICMP.ID != 7 || p.ICMP.Seq != 3 || string(p.ICMP.Data) != "ping" {
		t.Fatal(err, p.ICMP)
	}
}

func TestDHCPRoundTrip(t *testing.T) {
	disc := DHCP{Op: 1, XID: 0x1234, Flags: 0x8000, CHAddr: VMMAC,
		Options: []DHCPOpt{{OptMsgType, []byte{DHCPDiscover}}, {OptParamList, []byte{1, 3, 6}}}}
	d, err := ParseDHCP(disc.Bytes())
	if err != nil || d.MsgType() != DHCPDiscover || d.XID != 0x1234 || d.CHAddr != VMMAC || d.Flags != 0x8000 {
		t.Fatal(err, d)
	}
	srv, yi := IPv4(10, 0, 0, 1), IPv4(10, 0, 0, 2)
	off := DHCPReply(d, DHCPOffer, srv, yi, IPv4(255, 255, 255, 0), srv, srv, 3600)
	o, err := ParseDHCP(off.Bytes())
	if err != nil || o.MsgType() != DHCPOffer || o.YIAddr != yi {
		t.Fatal(err, o)
	}
	if s, ok := o.OptIP(OptServerID); !ok || s != srv {
		t.Fatal("server id")
	}
	if v, ok := o.Opt(OptLeaseTime); !ok || binary.BigEndian.Uint32(v) != 3600 {
		t.Fatal("lease")
	}
	// As a frame: client broadcast from 0.0.0.0.
	f := UDPFrame(Host{VMMAC, IP{}}, Host{Broadcast, IPv4(255, 255, 255, 255)},
		UDP{SrcPort: DHCPClientPort, DstPort: DHCPServerPort, Payload: disc.Bytes()})
	p, err := Decode(f)
	if err != nil || p.UDP == nil {
		t.Fatal(err)
	}
	if _, err := ParseDHCP(p.UDP.Payload); err != nil {
		t.Fatal(err)
	}
	b := disc.Bytes()
	if _, err := ParseDHCP(b[:len(b)-1]); err == nil {
		t.Fatal("options without End accepted")
	}
	b[237] = 0
	if _, err := ParseDHCP(b); err == nil {
		t.Fatal("bad cookie accepted")
	}
}

func TestDNSRoundTrip(t *testing.T) {
	q := DNS{ID: 77, Flags: DNSFlagRD, Questions: []DNSQuestion{{"example.com", DNSTypeA, DNSClassIN}}}
	g, err := ParseDNS(q.Bytes())
	if err != nil || g.ID != 77 || len(g.Questions) != 1 || g.Questions[0].Name != "example.com" {
		t.Fatal(err, g)
	}
	r := DNSAnswer(g, false, ARecord("example.com", IPv4(93, 184, 216, 34), 300))
	a, err := ParseDNS(r.Bytes())
	if err != nil || a.Flags&DNSFlagQR == 0 || a.Flags&DNSFlagRD == 0 || len(a.Answers) != 1 ||
		!bytes.Equal(a.Answers[0].Data, []byte{93, 184, 216, 34}) || a.Answers[0].TTL != 300 {
		t.Fatal(err, a)
	}
	nx, _ := ParseDNS(DNSAnswer(g, true).Bytes())
	if nx.Flags&0xF != DNSRcodeNX || len(nx.Answers) != 0 {
		t.Fatal(nx)
	}
	// A compressed answer name (pointer to the question name at 12).
	b := q.Bytes()
	binary.BigEndian.PutUint16(b[6:8], 1)
	b = append(b, 0xC0, 12, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 1, 2, 3, 4)
	c, err := ParseDNS(b)
	if err != nil || c.Answers[0].Name != "example.com" || c.Answers[0].TTL != 60 {
		t.Fatal(err, c)
	}
	// A pointer loop is rejected.
	loop := append([]byte(nil), b[:12]...)
	binary.BigEndian.PutUint16(loop[4:6], 1)
	binary.BigEndian.PutUint16(loop[6:8], 0)
	loop = append(loop, 0xC0, 12, 0, 1, 0, 1)
	if _, err := ParseDNS(loop); err == nil {
		t.Fatal("pointer loop accepted")
	}
}

// fakeVM answers the link protocol like the VM: on each advance it sends
// one frame naming the new time, then ACKs; exits after reaching 'stop'.
func fakeVM(c net.Conn, stop uint32) {
	defer c.Close()
	var now uint32
	for {
		typ, p, err := ReadMsg(c)
		if err != nil {
			return
		}
		if typ != MsgAdvance {
			continue
		}
		now += binary.BigEndian.Uint32(p)
		WriteMsg(c, MsgFrame, U32(now))
		if now >= stop {
			return // exits without an ACK
		}
		WriteMsg(c, MsgAck, nil)
	}
}

func TestLinkAdvance(t *testing.T) {
	a, b := net.Pipe()
	go fakeVM(b, 3000)
	l := NewLink(a)
	l.Timeout = 2 * time.Second
	fr, err := l.Advance(1000)
	if err != nil || len(fr) != 1 || binary.BigEndian.Uint32(fr[0]) != 1000 || l.Now() != 1000 {
		t.Fatal(err, fr)
	}
	fr, err = l.Advance(2000)
	if err != io.EOF || len(fr) != 1 || binary.BigEndian.Uint32(fr[0]) != 3000 || !l.Closed() {
		t.Fatal(err, fr)
	}
	if _, err := l.Advance(1); err != io.EOF {
		t.Fatal(err)
	}
}

// bufConn is an in-memory net.Conn: Write appends to out (never read back),
// Read drains in and then returns io.EOF (the peer ended). Separate buffers,
// so a message the simulator writes is not read back as the VM's.
type bufConn struct{ in, out *bytes.Buffer }

func (c *bufConn) Read(p []byte) (int, error) {
	if c.in.Len() == 0 {
		return 0, io.EOF
	}
	return c.in.Read(p)
}
func (c *bufConn) Write(p []byte) (int, error)     { return c.out.Write(p) }
func (c *bufConn) Close() error                    { return nil }
func (c *bufConn) LocalAddr() net.Addr             { return nil }
func (c *bufConn) RemoteAddr() net.Addr            { return nil }
func (c *bufConn) SetDeadline(time.Time) error     { return nil }
func (c *bufConn) SetReadDeadline(time.Time) error { return nil }
func (c *bufConn) SetWriteDeadline(time.Time) error { return nil }

// exitedVM is a link whose VM end wrote frames and then exited (closed)
// before the simulator's next message: the write to it fails, and the
// frames must still come back (otherwise the reply
// would be lost when the VM exited first).
func exitedVM(t *testing.T, frames ...string) *Link {
	// What the "VM" wrote, then EOF: the buffer holds it for the simulator's
	// end to read (a POSIX socketpair behaved this way; net.Pipe would
	// block, being unbuffered).
	vm := &bufConn{in: new(bytes.Buffer), out: new(bytes.Buffer)}
	for _, f := range frames {
		WriteMsg(vm, MsgFrame, []byte(f))
	}
	mine := &bufConn{
		in:  bytes.NewBuffer(append([]byte(nil), vm.out.Bytes()...)),
		out: new(bytes.Buffer),
	}
	l := NewLink(mine)
	l.Timeout = 2 * time.Second
	return l
}

func TestLinkFramesBeforeExit(t *testing.T) {
	for _, op := range []string{"advance", "setlink"} {
		l := exitedVM(t, "one", "two")
		var fr [][]byte
		var err error
		for i := 0; i < 3 && len(fr) == 0 && err == nil; i++ {
			// The first write to a closed peer may still succeed (it is
			// buffered); then the read path returns the frames.
			if op == "advance" {
				fr, err = l.Advance(0)
			} else {
				fr, err = l.SetLink(true)
			}
		}
		if err != io.EOF || len(fr) != 2 || string(fr[0]) != "one" || string(fr[1]) != "two" || !l.Closed() {
			t.Fatalf("%s: %d frames, err %v", op, len(fr), err)
		}
	}
}

func TestLinkTimeout(t *testing.T) {
	a, b := net.Pipe()
	defer b.Close()
	go io.Copy(io.Discard, b) // never answers
	l := NewLink(a)
	l.Timeout = 50 * time.Millisecond
	if _, err := l.Sync(); err != ErrTimeout {
		t.Fatal(err)
	}
}

func TestMsgFraming(t *testing.T) {
	var buf bytes.Buffer
	WriteMsg(&buf, MsgFrame, []byte{1, 2, 3})
	WriteMsg(&buf, MsgAck, nil)
	if !bytes.Equal(buf.Bytes(), []byte{0, 3, 0, 1, 2, 3, 0, 0, 2}) {
		t.Fatalf("% x", buf.Bytes())
	}
	typ, p, err := ReadMsg(&buf)
	if err != nil || typ != MsgFrame || !bytes.Equal(p, []byte{1, 2, 3}) {
		t.Fatal(typ, p, err)
	}
	typ, p, err = ReadMsg(&buf)
	if err != nil || typ != MsgAck || len(p) != 0 {
		t.Fatal(typ, p, err)
	}
	if _, _, err := ReadMsg(bytes.NewReader([]byte{0, 5, 0, 1})); err != io.ErrUnexpectedEOF {
		t.Fatal(err)
	}
	if WriteMsg(io.Discard, MsgFrame, make([]byte, 70000)) == nil {
		t.Fatal("oversized message")
	}
}
