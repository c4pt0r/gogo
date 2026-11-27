package disttest

// A node written in Go that speaks the wire protocol itself (handshake,
// framing, MACs, terms), to test the server's C codec and its defences
// with frames no gogo program would send: malformed and hostile ones.

import (
	"bytes"
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/binary"
	"fmt"
	"io"
	mrand "math/rand"
	"net"
	"os"
	"strconv"
	"testing"
	"time"
)

// Frame types and term tags (runtime/dist/dist.c).
const (
	fSend  = 1
	fReply = 2
	tagLen = 16

	tNil, tFalse, tTrue, tInt, tInt64, tString, tBytes, tByteSlice, tSlice           = 0, 1, 2, 3, 4, 5, 6, 7, 8
	tNilSlice, tStruct, tPID, tZeroPID, tRef, tZeroRef, tEmptyStr, tEmptyBytes, tOwn = 9, 10, 11, 12, 13, 14, 15, 16, 17
)

type peer struct {
	t        *testing.T
	c        net.Conn
	key      []byte
	self     uint16
	server   uint16
	tx, rx   uint32 // the last frame number sent; the highest received
	buf      []byte
	serial   uint64
	calledBy uint32 // the PID term replies come to
}

func mac(key []byte, parts ...[]byte) []byte {
	m := hmac.New(sha256.New, key)
	for _, p := range parts {
		m.Write(p)
	}
	return m.Sum(nil)
}

func le16(v int) []byte { return []byte{byte(v), byte(v >> 8)} }

func (p *peer) readRaw() []byte {
	p.t.Helper()
	for {
		if len(p.buf) >= 2 {
			n := int(binary.LittleEndian.Uint16(p.buf))
			if len(p.buf) >= 2+n {
				f := append([]byte(nil), p.buf[2:2+n]...)
				p.buf = p.buf[2+n:]
				return f
			}
		}
		b := make([]byte, 4096)
		k, err := p.c.Read(b)
		if err != nil {
			return nil
		}
		p.buf = append(p.buf, b[:k]...)
	}
}

// drain moves what the server has sent so far into p.buf without
// waiting (frames are checked when read), so a peer the test is not
// reading yet does not back the server's queue to it up.
func (p *peer) drain() {
	p.c.SetReadDeadline(time.Now().Add(time.Millisecond))
	defer p.c.SetReadDeadline(time.Now().Add(20 * time.Second))
	b := make([]byte, 65536)
	for {
		k, err := p.c.Read(b)
		p.buf = append(p.buf, b[:k]...)
		if err != nil {
			return
		}
	}
}

// dial completes a handshake as node name with token.
func dial(t *testing.T, port int, name, token string) (*peer, error) {
	c, err := net.Dial("tcp", fmt.Sprintf("127.0.0.1:%d", port))
	if err != nil {
		return nil, err
	}
	c.SetDeadline(time.Now().Add(20 * time.Second))
	p := &peer{t: t, c: c}
	ch := p.readRaw()
	if len(ch) != 18 || ch[0] != 0x40 {
		return nil, fmt.Errorf("challenge %x", ch)
	}
	ns := ch[2:18]
	nc := make([]byte, 16)
	rand.Read(nc)
	hello := append([]byte{0x41, 1}, nc...)
	hello = append(hello, mac([]byte(token), []byte("gogo-node-c"), ns, nc, []byte(name))...)
	hello = append(hello, name...)
	c.Write(append(le16(len(hello)), hello...))
	w := p.readRaw()
	if len(w) < 38 || w[0] != 0x42 {
		return nil, fmt.Errorf("welcome %x", w)
	}
	server, self := binary.LittleEndian.Uint16(w[2:]), binary.LittleEndian.Uint16(w[4:])
	sname := w[38:]
	ids := append(le16(int(self)), le16(int(server))...)
	if !hmac.Equal(w[6:38], mac([]byte(token), []byte("gogo-node-s"), nc, ns, ids, sname, []byte(name))) {
		return nil, fmt.Errorf("server MAC")
	}
	p.key = mac([]byte(token), []byte("gogo-node-k"), ns, nc, ids, sname, []byte(name))
	p.self, p.server = self, server
	p.calledBy = uint32(1<<20 | 7<<4 | 6) // a PID term of this node
	return p, nil
}

// send sends one frame body (header included), numbered and MACed.
func (p *peer) send(body []byte) {
	p.tx++
	p.sendNum(p.tx, body)
}

// sendNum sends body as frame number num.
func (p *peer) sendNum(num uint32, body []byte) {
	n := binary.LittleEndian.AppendUint32(nil, num)
	tag := mac(p.key, n, le16(int(p.self)), body)[:tagLen]
	f := append(le16(4+len(body)+tagLen), n...)
	f = append(f, body...)
	p.c.Write(append(f, tag...))
}

// recv returns the next frame body from the server (MAC checked), or nil
// after ms.
func (p *peer) recv(ms int) []byte {
	p.c.SetReadDeadline(time.Now().Add(time.Duration(ms) * time.Millisecond))
	defer p.c.SetReadDeadline(time.Now().Add(20 * time.Second))
	f := p.readRaw()
	if f == nil {
		return nil
	}
	body, ok := p.check(f)
	if !ok {
		p.t.Fatalf("bad frame from the server")
	}
	return body
}

// check verifies frame f (number, body, MAC) from the server: its MAC
// under the server's id, and a number above the last one.
func (p *peer) check(f []byte) ([]byte, bool) {
	if len(f) < 4+tagLen {
		return nil, false
	}
	num, body, tag := f[:4], f[4:len(f)-tagLen], f[len(f)-tagLen:]
	n := binary.LittleEndian.Uint32(num)
	ok := hmac.Equal(tag, mac(p.key, num, le16(int(p.server)), body)[:tagLen]) && n > p.rx
	p.rx = n
	return body, ok
}

func hdr(typ byte, dst, src uint16) []byte {
	return append([]byte{typ, 0}, append(le16(int(dst)), le16(int(src))...)...)
}

func varint(v uint64) []byte {
	var b []byte
	for v >= 0x80 {
		b = append(b, byte(v)|0x80)
		v >>= 7
	}
	return append(b, byte(v))
}

func zz(v int64) uint64 { return uint64(v<<1) ^ uint64(v>>63) }

func tInt32(v int32) []byte {
	return append([]byte{tInt}, varint(uint64(uint32(v<<1)^uint32(v>>31)))...)
}
func tStr(s string) []byte { return append(append([]byte{tString}, varint(uint64(len(s)))...), s...) }
func tPid(node uint16, term uint32) []byte {
	b := append([]byte{tPID}, varint(uint64(node))...)
	return binary.LittleEndian.AppendUint32(b, term)
}
func tRefOf(node uint16, serial uint64) []byte {
	return append(append([]byte{tRef}, varint(uint64(node))...), varint(serial)...)
}
func own(name string, fields ...[]byte) []byte {
	b := append([]byte{tOwn}, varint(uint64(len(fields)))...)
	b = append(append(b, varint(uint64(len(name)))...), name...)
	for _, f := range fields {
		b = append(b, f...)
	}
	return b
}

// request is an actor.Request{From: our PID, Ref: a fresh Ref of ours, Body}.
func (p *peer) request(body []byte) ([]byte, uint64) {
	p.serial++
	b := append([]byte{tStruct}, varint(4)...) // TYPE_REQUEST
	b = append(b, varint(3)...)
	b = append(b, tPid(p.self, p.calledBy)...)
	b = append(b, tRefOf(p.self, p.serial)...)
	return append(b, body...), p.serial
}

// call sends a Request to dst (0: the door) and waits ms for its reply's
// term (nil: none).
func (p *peer) call(dst uint32, body []byte, ms int) []byte {
	p.t.Helper()
	req, serial := p.request(body)
	f := hdr(fSend, p.server, p.self)
	f = binary.LittleEndian.AppendUint32(f, dst)
	p.send(append(f, req...))
	deadline := time.Now().Add(time.Duration(ms) * time.Millisecond)
	for time.Now().Before(deadline) {
		r := p.recv(int(time.Until(deadline)/time.Millisecond) + 1)
		if r == nil {
			return nil
		}
		if r[0] == fReply && len(r) >= 18 && binary.LittleEndian.Uint64(r[10:]) == serial {
			return r[18:]
		}
	}
	return nil
}

// whereis asks the server's door for name: the PID term on the server, or 0.
func (p *peer) whereis(name string) uint32 {
	p.t.Helper()
	r := p.call(0, own("node.whereisReq", tStr(name)), 3000)
	if len(r) == 6 && r[0] == tPID && r[1] == byte(p.server) {
		return binary.LittleEndian.Uint32(r[2:])
	}
	if len(r) == 1 && r[0] == tZeroPID {
		return 0
	}
	p.t.Fatalf("whereis %s: reply %x", name, r)
	return 0
}

func startServer(t *testing.T) (*node, int) {
	port := freePort(t)
	srv := start(t, "server", fmt.Sprintf("test.port=%d", port))
	srv.waitLine("listening")
	return srv, port
}

func noCrash(t *testing.T, srv *node) {
	t.Helper()
	srv.kill()
	if bytes.Contains([]byte(srv.errs), []byte("mailbox_overflow")) || bytes.Contains([]byte(srv.errs), []byte("ERROR")) ||
		bytes.Contains([]byte(srv.errs), []byte("runtime error")) {
		t.Errorf("server stderr:\n%s", srv.errs)
	}
}

// A peer cannot administer a node: the door answers whereis only, so
// unregister and register requests change nothing.
func TestPeerCannotAdminister(t *testing.T) {
	srv, port := startServer(t)
	p, err := dial(t, port, "mallory", token)
	if err != nil {
		t.Fatal(err)
	}
	echo := p.whereis("echo")
	if echo == 0 {
		t.Fatal("echo not found")
	}
	if r := p.call(0, own("node.unregisterReq", tStr("echo")), 500); r != nil {
		t.Errorf("unregister answered %x", r)
	}
	if r := p.call(0, own("node.registerReq", tStr("echo"), tPid(p.self, p.calledBy)), 500); r != nil {
		t.Errorf("register answered %x", r)
	}
	if r := p.call(0, own("node.assignReq", tStr("w"), tPid(p.self, p.calledBy)), 500); r != nil {
		t.Errorf("assign answered %x", r)
	}
	if got := p.whereis("echo"); got != echo {
		t.Errorf("echo is %x after a remote unregister/register, was %x", got, echo)
	}
	noCrash(t, srv)
}

// A 4 KB frame of nested empty slices (12 times its size in heap words)
// is refused, and kills nobody.
func TestAmplificationRefused(t *testing.T) {
	srv, port := startServer(t)
	p, err := dial(t, port, "mallory", token)
	if err != nil {
		t.Fatal(err)
	}
	n := 1990
	body := append([]byte{tSlice}, varint(uint64(n))...)
	for i := 0; i < n; i++ {
		body = append(body, tSlice, 0)
	}
	f := binary.LittleEndian.AppendUint32(hdr(fSend, p.server, p.self), 0)
	p.send(append(f, body...))
	echo := p.whereis("echo")
	big := append(append([]byte{tSlice}, varint(uint64(n))...), bytes.Repeat([]byte{tSlice, 0}, n)...)
	if r := p.call(echo, big, 500); r != nil {
		t.Errorf("echo answered the amplified term")
	}
	if p.whereis("echo") != echo {
		t.Error("the door is gone")
	}
	noCrash(t, srv)
}

// Every term tag goes to echo and comes back byte for byte.
func TestCodecRoundTrip(t *testing.T) {
	srv, port := startServer(t)
	p, err := dial(t, port, "mallory", token)
	if err != nil {
		t.Fatal(err)
	}
	echo := p.whereis("echo")
	terms := map[string][]byte{
		"nil": {tNil}, "false": {tFalse}, "true": {tTrue},
		"int": tInt32(-12345), "big int": tInt32(1 << 30), "min int": tInt32(-1 << 31),
		"int64":     append([]byte{tInt64}, varint(zz(-1<<62))...),
		"string":    tStr("héllo"),
		"empty str": {tEmptyStr},
		"bytes":     append(append([]byte{tBytes}, varint(70)...), bytes.Repeat([]byte{7}, 70)...),
		"empty b":   {tEmptyBytes},
		"[]byte":    append(append([]byte{tByteSlice}, varint(3)...), 1, 2, 3),
		"slice":     append(append([]byte{tSlice}, varint(3)...), append(append(tInt32(1), tStr("x")...), tTrue)...),
		"empty sl":  {tSlice, 0},
		"nil slice": {tNilSlice},
		"net.Addr":  append([]byte{tStruct, 13, 2}, append(tInt32(1), tInt32(2)...)...),
		"zero pid":  {tZeroPID},
		"zero ref":  {tZeroRef},
		"our pid":   tPid(p.self, p.calledBy),
		"our ref":   tRefOf(p.self, 99),
		"float32":   {0x13, 0, 0, 0xc0, 0x3f},
		"float64":   {0x14, 0, 0, 0, 0, 0, 0, 0xf8, 0x3f},
		"nested":    append(append([]byte{tSlice}, varint(1)...), append(append([]byte{tSlice}, varint(1)...), tInt32(5)...)...),
	}
	for name, term := range terms {
		if got := p.call(echo, term, 2000); !bytes.Equal(got, term) {
			t.Errorf("%s: sent %x, got back %x", name, term, got)
		}
	}
	noCrash(t, srv)
}

// Malformed and hostile terms are dropped (no reply), and the link and
// the server stay fine.
func TestMalformedTerms(t *testing.T) {
	srv, port := startServer(t)
	p, err := dial(t, port, "mallory", token)
	if err != nil {
		t.Fatal(err)
	}
	echo := p.whereis("echo")
	deep := []byte{}
	for i := 0; i < 17; i++ {
		deep = append(deep, tSlice, 1)
	}
	deep = append(deep, tNil)
	bad := map[string][]byte{
		"truncated varint":   {tInt, 0x80},
		"overlong int":       {tInt, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01},
		"int bits past 32":   {tInt, 0xff, 0xff, 0xff, 0xff, 0x7f},
		"int64 past 64 bits": append([]byte{tInt64}, append(bytes.Repeat([]byte{0xff}, 9), 0x7f)...),
		"string past end":    {tString, 10, 'a'},
		"bad tag":            {0x3f},
		"unknown tag 0x13":   {0x13},
		"reserved nf":        append([]byte{tStruct, 13, 3}, append(tInt32(1), append(tInt32(2), tInt32(3)...)...)...),
		"reserved id":        {tStruct, 120, 0},
		"depth 17":           deep,
		"slice longer":       {tSlice, 5, tNil},
		"own unknown":        own("nobody.Knows", tInt32(1)),
		"pid unknown node":   tPid(999, 1<<20|1<<4|6),
		"pid port slot":      tPid(p.self, 1<<20|0xFFF1<<4|6),
		"server's own ref":   tRefOf(p.server, 5),
		"trailing bytes":     {tNil, tNil},
	}
	for name, term := range bad {
		if got := p.call(echo, term, 300); got != nil {
			t.Errorf("%s: echoed %x", name, got)
		}
	}
	if p.whereis("echo") != echo {
		t.Error("the door is gone")
	}
	noCrash(t, srv)
}

// Random and mutated frames (valid MACs): nothing crashes, and the server
// still answers. $GOGO_DIST_FUZZ sets the frame count (default 2000).
func TestFuzzFrames(t *testing.T) {
	count := 2000
	if v := os.Getenv("GOGO_DIST_FUZZ"); v != "" {
		count, _ = strconv.Atoi(v)
	}
	srv, port := startServer(t)
	p, err := dial(t, port, "mallory", token)
	if err != nil {
		t.Fatal(err)
	}
	echo := p.whereis("echo")
	rng := mrand.New(mrand.NewSource(1))
	seeds := [][]byte{tInt32(5), tStr("abc"), own("main.Ping", tPid(p.self, p.calledBy), tInt32(1)),
		append([]byte{tStruct, 4, 3}, append(tPid(p.self, p.calledBy), append(tRefOf(p.self, 1), tNil)...)...)}
	for i := 0; i < count; i++ {
		var body []byte
		if rng.Intn(3) == 0 {
			body = make([]byte, 6+rng.Intn(60))
			rng.Read(body)
			body[0] = byte(rng.Intn(14))
		} else {
			term := append([]byte(nil), seeds[rng.Intn(len(seeds))]...)
			for k := rng.Intn(4); k >= 0; k-- {
				switch rng.Intn(3) {
				case 0:
					term[rng.Intn(len(term))] = byte(rng.Intn(256))
				case 1:
					term = term[:rng.Intn(len(term))+1]
				case 2:
					term = append(term, byte(rng.Intn(256)))
				}
			}
			dst := echo
			if rng.Intn(4) == 0 {
				dst = 0
			}
			body = binary.LittleEndian.AppendUint32(hdr(byte(1+rng.Intn(2)), p.server, p.self), dst)
			if body[0] == fReply {
				body = binary.LittleEndian.AppendUint64(body, uint64(rng.Intn(10)))
			}
			body = append(body, term...)
		}
		p.send(body)
		for p.recv(0) != nil {
		}
	}
	if p.whereis("echo") != echo {
		t.Error("the server stopped answering")
	}
	noCrash(t, srv)
}

// handshakeCap: a server started with flags takes slots connections into
// their handshake at once (each gets the challenge); the next one gets
// busy (0x44, version) and is closed, and the slots come back once the
// idle connections go.
func handshakeCap(t *testing.T, slots int, flags ...string) {
	port := freePort(t)
	srv := startVM(t, "server", flags, fmt.Sprintf("test.port=%d", port))
	srv.waitLine("listening")
	var idle []net.Conn
	for i := 0; i < slots; i++ {
		c, err := net.Dial("tcp", fmt.Sprintf("127.0.0.1:%d", port))
		if err != nil {
			t.Fatal(err)
		}
		defer c.Close()
		c.SetReadDeadline(time.Now().Add(5 * time.Second))
		b := make([]byte, 20)
		if _, err := io.ReadFull(c, b); err != nil || b[2] != 0x40 {
			t.Fatalf("connection %d of %d: no challenge (%x, %v)", i+1, slots, b, err)
		}
		idle = append(idle, c)
	}
	c, err := net.Dial("tcp", fmt.Sprintf("127.0.0.1:%d", port))
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	c.SetReadDeadline(time.Now().Add(2 * time.Second))
	b, err := io.ReadAll(c)
	if err != nil || !bytes.Equal(b, []byte{2, 0, 0x44, 1}) {
		t.Errorf("connection %d: got %x (%v), want busy and the end", slots+1, b, err)
	}
	for _, c := range idle {
		c.Close()
	}
	// The slots come back: a node can join again.
	time.Sleep(300 * time.Millisecond)
	if _, err := dial(t, port, "late", token); err != nil {
		t.Errorf("join after the idle connections closed: %v", err)
	}
	noCrash(t, srv)
}

// The handshake slots scale with the link cap: L / 32, at least 2 and at
// most 64.
func TestHandshakeCapHost(t *testing.T)  { handshakeCap(t, 16) }
func TestHandshakeCapBoard(t *testing.T) { handshakeCap(t, 2, "--max-links", "8") }
func TestHandshakeCapCeiling(t *testing.T) {
	handshakeCap(t, 64, "--max-ports", "2048", "--max-links", "2048")
}

// A board's Connect to a server whose slots are all taken fails with
// errors.Busy (the busy frame), not Refused: the error to retry on.
func TestConnectBusy(t *testing.T) {
	port := freePort(t)
	srv := startVM(t, "server", []string{"--max-links", "8"}, fmt.Sprintf("test.port=%d", port))
	srv.waitLine("listening")
	for i := 0; i < 2; i++ {
		c, err := net.Dial("tcp", fmt.Sprintf("127.0.0.1:%d", port))
		if err != nil {
			t.Fatal(err)
		}
		defer c.Close()
		c.SetReadDeadline(time.Now().Add(5 * time.Second))
		if _, err := io.ReadFull(c, make([]byte, 20)); err != nil {
			t.Fatalf("connection %d: no challenge: %v", i+1, err)
		}
	}
	b := start(t, "board_busy", fmt.Sprintf("test.server=127.0.0.1:%d", port))
	b.finish("connect busy true\nbusy\nself true\n")
	noCrash(t, srv)
}

// A frame with a wrong MAC ends the link.
func TestBadMACEndsLink(t *testing.T) {
	srv, port := startServer(t)
	p, err := dial(t, port, "mallory", token)
	if err != nil {
		t.Fatal(err)
	}
	p.whereis("echo")
	body := binary.LittleEndian.AppendUint32(hdr(fSend, p.server, p.self), 0)
	body = append(body, tNil)
	f := append(le16(4+len(body)+tagLen), 1, 0, 0, 0)
	f = append(f, body...)
	p.c.Write(append(f, make([]byte, tagLen)...))
	p.c.SetReadDeadline(time.Now().Add(3 * time.Second))
	if _, err := io.ReadAll(p.c); err != nil {
		if ne, ok := err.(net.Error); ok && ne.Timeout() {
			t.Errorf("link still open after a bad MAC")
		}
	}
	noCrash(t, srv)
}
