package nettest

import (
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"

	"gogotools/tests/net/sim"
)

// startHTTP starts progs/<name> with a static address (10.0.0.2/24,
// gateway and DNS server 10.0.0.1) and the extra config lines, and
// answers the gateway's ARP.
func startHTTP(t *testing.T, name, cfg string) *sim.VM {
	t.Helper()
	path := filepath.Join(t.TempDir(), "config")
	cfg = "net.ip=10.0.0.2\nnet.gw=10.0.0.1\nnet.dns=10.0.0.1\n" + cfg
	if err := os.WriteFile(path, []byte(cfg), 0o600); err != nil {
		t.Fatal(err)
	}
	t.Setenv("GOGO_CONFIG", path)
	v := start(t, name)
	arpFor(t, one(t)(v.Sync()), vmHost.IP, gwHost.IP)
	none(t)(v.Exchange(arpReply(gwHost, vmHost)))
	return v
}

// data takes this connection's segments: their payload in order and
// whether a FIN came, both counted as received; it checks their sequence
// numbers and that none is a RST.
func (c *tcpConn) data(s []sim.TCP) (string, bool) {
	c.t.Helper()
	out, fin := "", false
	for _, x := range s {
		if x.SrcPort != c.vport || x.DstPort != c.pport || x.Flags&sim.RST != 0 {
			c.t.Fatalf("unexpected %s", segStr(x))
		}
		if len(x.Payload) > 0 || x.Flags&sim.FIN != 0 {
			if x.Seq != c.ack {
				c.t.Fatalf("got %s, want seq %d", segStr(x), c.ack)
			}
			out += string(x.Payload)
			c.ack += uint32(len(x.Payload))
			if x.Flags&sim.FIN != 0 {
				c.ack++
				fin = true
			}
		}
	}
	return out, fin
}

// exchange sends a segment of c and sorts the VM's answer by peer port.
func (c *tcpConn) exchange(flags uint8, payload string) map[uint16][]sim.TCP {
	c.t.Helper()
	return split(c.send(flags, []byte(payload)))
}

// closeVM checks the VM's FIN is the only thing in s (after any data),
// then sends the peer's FIN and expects the VM's last ACK.
func (c *tcpConn) finish(s []sim.TCP) {
	c.t.Helper()
	if _, fin := c.data(s); !fin {
		c.t.Fatalf("want FIN, got %s", segsStr(s))
	}
	c.expect(c.mine(c.send(sim.FIN|sim.ACK, nil)), sim.ACK, "")
}

// rst expects s to be a RST of this connection, after at most one ACK.
func (c *tcpConn) rst(s []sim.TCP) {
	c.t.Helper()
	if len(s) == 2 && s[0].Flags == sim.ACK && len(s[0].Payload) == 0 {
		s = s[1:]
	}
	if len(s) != 1 || s[0].Flags&sim.RST == 0 || s[0].SrcPort != c.vport {
		c.t.Fatalf("want RST, got %s", segsStr(s))
	}
}

// dialed answers the VM's SYN in s (to port pport) and returns the
// request it sends at once (the whole request must come before any
// acknowledgement when it fits a segment) with the frames by port.
func dialed(t *testing.T, v *sim.VM, peer sim.Host, pport uint16, s []sim.TCP) (*tcpConn, string) {
	t.Helper()
	c := newConn(t, v, pport, 0)
	c.peer = peer
	c.accept(s)
	m := c.exchange(sim.SYN|sim.ACK, "")
	req, _ := c.data(m[pport])
	return c, req
}

// Get with a literal address: URLs that are not http:// or https://
// fail at once;
// the request is HTTP/1.0 with Host and Connection: close; a response
// split anywhere is reassembled; Content-Length (any case) ends the body
// before the server closes; without it the body runs to the close; the
// client closes gracefully after either; a close before Content-Length
// is complete fails.
func TestHTTPGet(t *testing.T) {
	v := startHTTP(t, "http_client",
		"test.script=GET:httpx://a.test/ GET:http://:80/ GET:http://h:99999/ GET:ftp://x sleep:100 GET:http://10.0.0.1:8080/index.html#top GET:http://10.0.0.1:81 GET:http://10.0.0.1:83/short\n")
	a, req := dialed(t, v, gwHost, 8080, split(newConn(t, v, 0, 0).segs(v.Advance(100)))[8080])
	if want := "GET /index.html HTTP/1.0\r\nHost: 10.0.0.1:8080\r\nConnection: close\r\n\r\n"; req != want {
		t.Fatalf("request %q, want %q", req, want)
	}
	none(t)(v.Exchange(a.frame(sim.ACK, nil)))
	m := a.exchange(sim.PSH|sim.ACK, "HTTP/1.0 200 OK\r\ncontent-length: 5\r\nX-A:  one \r")
	a.expect(m[8080], sim.ACK, "")
	m = a.exchange(sim.PSH|sim.ACK, "\nX-B:\r\n\r\nhel")
	a.expect(m[8080], sim.ACK, "")
	m = a.exchange(sim.PSH|sim.ACK, "lo")
	a.finish(m[8080]) // Content-Length complete: FIN
	b, req := dialed(t, v, gwHost, 81, m[81])
	if want := "GET / HTTP/1.0\r\nHost: 10.0.0.1:81\r\nConnection: close\r\n\r\n"; req != want {
		t.Fatalf("request %q, want %q", req, want)
	}
	m = b.exchange(sim.PSH|sim.ACK, "HTTP/1.1 404 Not Found\r\nServer: sim\r\n\r\nno such page")
	b.expect(m[81], sim.ACK, "")
	m = b.exchange(sim.FIN|sim.ACK, "")
	if _, fin := b.data(m[81]); !fin {
		t.Fatalf("want FIN, got %s", segsStr(m[81]))
	}
	none(t)(v.Exchange(b.frame(sim.ACK, nil)))
	// Closed before Content-Length is complete: a failure, reset.
	c, _ := dialed(t, v, gwHost, 83, m[83])
	c.expect(c.mine(c.send(sim.PSH|sim.ACK, []byte("HTTP/1.0 200 OK\r\nContent-Length: 20\r\n\r\nshort"))), sim.ACK, "")
	c.rst(c.mine(c.send(sim.FIN|sim.ACK, nil)))
	stop(t, v, "start true\n"+
		"GET httpx://a.test/ false 0 0 false 0\n  body []\n"+
		"GET http://:80/ false 0 0 false 0\n  body []\n"+
		"GET http://h:99999/ false 0 0 false 0\n  body []\n"+
		"GET ftp://x false 0 0 false 0\n  body []\n"+
		"GET http://10.0.0.1:8080/index.html#top true 200 5 false 100\n  content-length: 5\n  X-A: one\n  X-B: \n  body [hello]\n"+
		"GET http://10.0.0.1:81 true 404 12 false 100\n  Server: sim\n  body [no such page]\n"+
		"GET http://10.0.0.1:83/short false 0 0 false 100\n  body []\n"+
		"done\n")
}

// Get by name: the host is resolved through the DNS server (an
// off-subnet answer is reached through the gateway) and sent as Host; a
// name that does not resolve fails.
func TestHTTPGetDNS(t *testing.T) {
	v := startHTTP(t, "http_client", "test.script=sleep:100 GET:http://www.example.test/x GET:http://nx.test/\n")
	q, port := dnsQuery(t, one(t)(v.Advance(100)), gwHost.IP, "www.example.test")
	far := sim.Host{MAC: gwHost.MAC, IP: farIP}
	syn := newConn(t, v, 0, 0)
	syn.peer = far
	s := syn.segs(v.Exchange(dnsFrame(gwHost, port, sim.DNSAnswer(q, false, sim.ARecord("www.example.test", farIP, 60)).Bytes())))
	c, req := dialed(t, v, far, 80, s)
	if want := "GET /x HTTP/1.0\r\nHost: www.example.test\r\nConnection: close\r\n\r\n"; req != want {
		t.Fatalf("request %q, want %q", req, want)
	}
	// The response: the ACK and FIN, then the next lookup's query.
	resp := "HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nhi"
	fr, err := v.Exchange(c.frame(sim.PSH|sim.ACK, []byte(resp)))
	c.seq += uint32(len(resp))
	if err != nil || len(fr) != 3 {
		t.Fatalf("%v: %d frames, want ACK, FIN and a query", err, len(fr))
	}
	c.finish(c.segs(fr[:2], nil)[1:])
	q, port = dnsQuery(t, decode(t, fr[2]), gwHost.IP, "nx.test")
	none(t)(v.Exchange(dnsFrame(gwHost, port, sim.DNSAnswer(q, true).Bytes())))
	stop(t, v, "start true\n"+
		"GET http://www.example.test/x true 200 2 false 100\n  Content-Length: 2\n  body [hi]\n"+
		"GET http://nx.test/ false 0 0 false 100\n  body []\n"+
		"done\n")
}

// Post: Content-Type and Content-Length; a body longer than the peer's
// MSS arrives in order over several segments, after the head.
func TestHTTPPost(t *testing.T) {
	text := alpha(1000)
	v := startHTTP(t, "http_client", "test.script=sleep:100 POST:http://10.0.0.1/api\ntest.body="+text+"\n")
	c := newConn(t, v, 80, 0)
	c.accept(c.segs(v.Advance(100)))
	req, _ := c.data(c.mine(c.send(sim.SYN|sim.ACK, nil, sim.MSSOption(536)...)))
	for i := 0; i < 5 && len(req) < 1105; i++ {
		got, _ := c.data(c.mine(c.send(sim.ACK, nil)))
		req += got
	}
	if want := "POST /api HTTP/1.0\r\nHost: 10.0.0.1\r\nConnection: close\r\nContent-Type: text/plain\r\nContent-Length: 1000\r\n\r\n" + text; req != want {
		t.Fatalf("request %q, want %q", req, want)
	}
	none(t)(v.Exchange(c.frame(sim.ACK, nil)))
	c.finish(c.mine(c.send(sim.PSH|sim.ACK, []byte("HTTP/1.0 201 Created\r\nContent-Length: 2\r\n\r\nok")))[1:])
	stop(t, v, "start true\nPOST http://10.0.0.1/api true 201 2 false 100\n  Content-Length: 2\n  body [ok]\ndone\n")
}

// A server that accepts but never answers, and one that never accepts:
// Get fails at its timeout; the abandoned connection is reset (nothing is
// sent for one still connecting).
func TestHTTPTimeout(t *testing.T) {
	v := startHTTP(t, "http_client", "test.script=sleep:100 GET:http://10.0.0.1/slow GET:http://10.0.0.1:82/\ntest.timeout=2000\n")
	c, req := dialed(t, v, gwHost, 80, newConn(t, v, 0, 0).segs(v.Advance(100)))
	if !strings.HasPrefix(req, "GET /slow ") {
		t.Fatalf("request %q", req)
	}
	none(t)(v.Exchange(c.frame(sim.ACK, nil)))
	none(t)(v.Advance(1999))
	m := split(c.segs(v.Advance(1)))
	c.rst(m[80])
	d := newConn(t, v, 82, 0)
	syn := d.accept(m[82])
	expectRetx(t, d.segs(v.Advance(1000)), syn)
	none(t)(v.Advance(1000))
	stop(t, v, "start true\n"+
		"GET http://10.0.0.1/slow false 0 0 false 2100\n  body []\n"+
		"GET http://10.0.0.1:82/ false 0 0 false 4100\n  body []\n"+
		"done\n")
}

// pattern is n body bytes starting at offset off.
func pattern(off, n int) []byte {
	b := make([]byte, n)
	for i := range b {
		b[i] = byte('0' + (off+i)%61)
	}
	return b
}

func byteSum(b []byte) int {
	s := 0
	for _, x := range b {
		s += int(x)
	}
	return s
}

// Bodies over the limit are cut and marked Truncated, the connection
// reset: MaxBody (8 KB) with a longer Content-Length; a per-request limit
// with a body to the close; a body within the limit is not truncated.
func TestHTTPBodyCap(t *testing.T) {
	v := startHTTP(t, "http_client", "test.script=sleep:100 GET:http://10.0.0.1/big max:100 GET:http://10.0.0.1:81/small GET:http://10.0.0.1:82/nolen\n")
	c, _ := dialed(t, v, gwHost, 80, newConn(t, v, 0, 0).segs(v.Advance(100)))
	c.expect(c.mine(c.send(sim.PSH|sim.ACK, []byte("HTTP/1.0 200 OK\r\nContent-Length: 10000\r\n\r\n"))), sim.ACK, "")
	var m map[uint16][]sim.TCP
	for off := 0; off < 8400; off += 1400 {
		m = split(c.send(sim.PSH|sim.ACK, pattern(off, 1400)))
		if off+1400 < 8192 {
			c.expect(m[80], sim.ACK, "")
		}
	}
	c.rst(m[80]) // 8400 > 8192: cut
	d, _ := dialed(t, v, gwHost, 81, m[81])
	m = d.exchange(sim.PSH|sim.ACK, "HTTP/1.0 200 OK\r\nContent-Length: 50\r\n\r\n"+string(pattern(0, 50)))
	d.finish(m[81][1:])
	e, _ := dialed(t, v, gwHost, 82, m[82])
	e.expect(e.mine(e.send(sim.PSH|sim.ACK, []byte("HTTP/1.0 200 OK\r\n\r\n"))), sim.ACK, "")
	e.rst(e.mine(e.send(sim.PSH|sim.ACK, pattern(0, 300))))
	stop(t, v, "start true\n"+
		"GET http://10.0.0.1/big true 200 8192 true 100\n  Content-Length: 10000\n  sum "+strconv.Itoa(byteSum(pattern(0, 8192)))+"\n"+
		"GET http://10.0.0.1:81/small true 200 50 false 100\n  Content-Length: 50\n  body ["+string(pattern(0, 50))+"]\n"+
		"GET http://10.0.0.1:82/nolen true 200 100 true 100\n  sum "+strconv.Itoa(byteSum(pattern(0, 100)))+"\n"+
		"done\n")
}

// browse opens a browser connection from pport to the server's port 80:
// handshake, Accepted, handed off (a window update).
func browse(t *testing.T, v *sim.VM, pport uint16) *tcpConn {
	t.Helper()
	c := newConn(t, v, pport, 80)
	s := c.send(sim.SYN, nil, sim.MSSOption(1460)...)
	if len(s) != 1 || s[0].Flags != sim.SYN|sim.ACK || s[0].Window != 0 {
		t.Fatalf("want SYN-ACK with window 0, got %s", segsStr(s))
	}
	c.ack = s[0].Seq + 1
	if x := c.expect(c.send(sim.ACK, nil), sim.ACK, ""); x.Window != 1460 {
		t.Fatalf("window %d after handoff", x.Window)
	}
	return c
}

// fetch sends the request segments and acknowledges the response until
// the server's FIN; it returns the response (s: the answer to the last
// request segment, for this connection) and completes the close.
func (c *tcpConn) fetch(segs ...string) string {
	c.t.Helper()
	var s []sim.TCP
	for i, seg := range segs {
		s = c.mine(c.send(sim.PSH|sim.ACK, []byte(seg)))
		if i < len(segs)-1 {
			c.expect(s, sim.ACK, "")
		}
	}
	resp := ""
	for i := 0; ; i++ {
		got, fin := c.data(s)
		resp += got
		if fin {
			break
		}
		if i == 20 || len(s) == 0 {
			c.t.Fatalf("no FIN; response so far %q", resp)
		}
		s = c.mine(c.send(sim.ACK, nil))
	}
	c.expect(c.mine(c.send(sim.FIN|sim.ACK, nil)), sim.ACK, "")
	return resp
}

const okHead = "HTTP/1.0 200 OK\r\nContent-Length: "

// Serve: a second listener on the port fails (its caller gets the exit);
// a GET split over segments is answered with the handler's response,
// Content-Length and Connection: close; a POST body over several segments
// reaches the handler, whose own Content-Length is replaced; the server
// closes first and the connection finishes after the handler exits.
func TestHTTPServe(t *testing.T) {
	v := startHTTP(t, "http_server", "")
	a := browse(t, v, 40001)
	resp := a.fetch("GET /hi HTTP/1.1\r\nHost: dev", "\r\nUser-Agent: sim/1\r\n\r\n")
	if want := okHead + "9\r\nConnection: close\r\nContent-Type: text/plain\r\n\r\nhello /hi"; resp != want {
		t.Fatalf("response %q, want %q", resp, want)
	}
	body := alpha(3000)
	b := browse(t, v, 40002)
	req := "POST /echo HTTP/1.0\r\nContent-Length: 3000\r\n\r\n" + body
	resp = b.fetch(req[:1460], req[1460:2920], req[2920:])
	if want := "HTTP/1.0 201 Created\r\nContent-Length: 3000\r\nConnection: close\r\nContent-Type: text/plain\r\n\r\n" + body; resp != want {
		t.Fatalf("response %q, want %q", resp, want)
	}
	// A body of exactly MaxBody.
	body = alpha(8192)
	c := browse(t, v, 40003)
	req = "POST /max HTTP/1.0\r\nContent-Length: 8192\r\n\r\n" + body
	var segs []string
	for len(req) > 1460 {
		segs, req = append(segs, req[:1460]), req[1460:]
	}
	resp = c.fetch(append(segs, req)...)
	if want := "HTTP/1.0 201 Created\r\nContent-Length: 8192\r\nConnection: close\r\nContent-Type: text/plain\r\n\r\n" + body; resp != want {
		t.Fatalf("response of %d bytes, want %d", len(resp), len(want))
	}
	stop(t, v, "start true\nexit 69\nreq GET /hi 2 sim/1 0 0\nreq POST /echo 1  3000 0\nreq POST /max 1  8192 0\n")
}

const badRequest = "HTTP/1.0 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"

// Malformed requests get 400 without reaching the handler: a bad request
// line, a header without a colon, a bad Content-Length, a chunked body,
// a head over 2 KB; a body over MaxBody gets 413. A connection that sends
// nothing is closed after ReadTimeoutMs; one the peer closes first is
// closed too.
func TestHTTPServeMalformed(t *testing.T) {
	v := startHTTP(t, "http_server", "")
	for i, req := range []string{
		"GARBAGE\r\n\r\n",
		"GET nopath HTTP/1.0\r\n\r\n",
		"GET / HTTP/1.0\r\nBroken header\r\n\r\n",
		"POST / HTTP/1.0\r\nContent-Length: x\r\n\r\n",
		"POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n",
	} {
		c := browse(t, v, uint16(41000+i))
		if resp := c.fetch(req); resp != badRequest {
			t.Fatalf("%q: response %q", req, resp)
		}
	}
	c := browse(t, v, 41100)
	if resp := c.fetch("GET / HTTP/1.0\r\nX: "+alpha(1400), alpha(700)); resp != badRequest {
		t.Fatalf("long head: response %q", resp)
	}
	c = browse(t, v, 41101)
	if resp := c.fetch("POST / HTTP/1.0\r\nContent-Length: 8193\r\n\r\n"); resp != "HTTP/1.0 413 Payload Too Large\r\nContent-Length: 0\r\nConnection: close\r\n\r\n" {
		t.Fatalf("big body: response %q", resp)
	}
	c = browse(t, v, 41102)
	none(t)(v.Advance(9999))
	c.finish(c.segs(v.Advance(1)))
	c = browse(t, v, 41103)
	if _, fin := c.data(c.mine(c.send(sim.FIN|sim.ACK, nil))); !fin {
		t.Fatal("peer closed first: want FIN")
	}
	none(t)(v.Exchange(c.frame(sim.ACK, nil)))
	stop(t, v, "start true\nexit 69\n")
}

// A handler that crashes resets its connection; the listener and later
// requests are unaffected.
func TestHTTPServeHandlerCrash(t *testing.T) {
	v := startHTTP(t, "http_server", "")
	a := browse(t, v, 40001)
	a.rst(a.mine(a.send(sim.PSH|sim.ACK, []byte("GET /crash HTTP/1.0\r\n\r\n"))))
	b := browse(t, v, 40002)
	if resp := b.fetch("GET /after HTTP/1.0\r\n\r\n"); !strings.HasSuffix(resp, "\r\n\r\nhello /after") {
		t.Fatalf("response %q", resp)
	}
	stop(t, v, "start true\nexit 69\nreq GET /crash 0  0 0\nreq GET /after 0  0 0\n")
	if !strings.Contains(v.Stderr.String(), "divzero") {
		t.Errorf("stderr %q, want the crash report", v.Stderr.String())
	}
}

// Two browsers at once while one handler makes an outbound request (3
// connections of 4): the second browser is answered while the first
// waits for the proxied response; then the first gets it.
func TestHTTPServeTwoBrowsers(t *testing.T) {
	v := startHTTP(t, "http_server", "")
	a := browse(t, v, 40001)
	b := browse(t, v, 40002)
	m := a.exchange(sim.PSH|sim.ACK, "GET /proxy HTTP/1.0\r\n\r\n")
	a.expect(m[40001], sim.ACK, "")
	p, req := dialed(t, v, gwHost, 9000, m[9000])
	if want := "GET /data HTTP/1.0\r\nHost: 10.0.0.1:9000\r\nConnection: close\r\n\r\n"; req != want {
		t.Fatalf("proxy request %q, want %q", req, want)
	}
	none(t)(v.Exchange(p.frame(sim.ACK, nil)))
	if resp := b.fetch("GET /b HTTP/1.0\r\n\r\n"); resp != okHead+"8\r\nConnection: close\r\nContent-Type: text/plain\r\n\r\nhello /b" {
		t.Fatalf("browser b: response %q", resp)
	}
	m = p.exchange(sim.PSH|sim.ACK, "HTTP/1.0 200 OK\r\nContent-Length: 4\r\n\r\ndata")
	p.finish(m[9000][1:])
	resp, _ := a.data(m[40001])
	for i := 0; i < 5; i++ {
		got, fin := a.data(a.mine(a.send(sim.ACK, nil)))
		resp += got
		if fin {
			break
		}
	}
	if want := okHead + "4\r\nConnection: close\r\n\r\ndata"; resp != want {
		t.Fatalf("browser a: response %q, want %q", resp, want)
	}
	a.expect(a.mine(a.send(sim.FIN|sim.ACK, nil)), sim.ACK, "")
	stop(t, v, "start true\nexit 69\nreq GET /proxy 0  0 0\nreq GET /b 0  0 0\nproxy true 200 0\n")
}

// Responses the client rejects (the connection reset): chunked (not
// decoded), two different Content-Lengths, a negative one. URLs with user
// info or a method with a control byte fail at once; a query with no path
// is sent as "/?query".
func TestHTTPGetMalformed(t *testing.T) {
	v := startHTTP(t, "http_client",
		"test.script=GET:http://u@10.0.0.1/ GE\tT:http://10.0.0.1/ sleep:100 GET:http://10.0.0.1:84?q=1#f GET:http://10.0.0.1:85/ GET:http://10.0.0.1:86/\n")
	c, req := dialed(t, v, gwHost, 84, newConn(t, v, 0, 0).segs(v.Advance(100)))
	if want := "GET /?q=1 HTTP/1.0\r\nHost: 10.0.0.1:84\r\nConnection: close\r\n\r\n"; req != want {
		t.Fatalf("request %q, want %q", req, want)
	}
	m := c.exchange(sim.PSH|sim.ACK, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n")
	c.rst(m[84])
	for _, p := range []struct {
		port uint16
		head string
	}{{85, "Content-Length: 5\r\ncontent-length: 6"}, {86, "Content-Length: -1"}} {
		c, _ = dialed(t, v, gwHost, p.port, m[p.port])
		m = c.exchange(sim.PSH|sim.ACK, "HTTP/1.0 200 OK\r\n"+p.head+"\r\n\r\nhello")
		c.rst(m[p.port])
	}
	stop(t, v, "start true\n"+
		"GET http://u@10.0.0.1/ false 0 0 false 0\n  body []\n"+
		"GE\tT http://10.0.0.1/ false 0 0 false 0\n  body []\n"+
		"GET http://10.0.0.1:84?q=1#f false 0 0 false 100\n  body []\n"+
		"GET http://10.0.0.1:85/ false 0 0 false 100\n  body []\n"+
		"GET http://10.0.0.1:86/ false 0 0 false 100\n  body []\n"+
		"done\n")
}

// HEAD gets the head only (Content-Length of the body it would get); a
// request with two different Content-Lengths or a control byte in the
// path gets 400; a handler header with CRLF in it turns the response into
// a 500 (no response splitting).
func TestHTTPServeHeadAndSplitting(t *testing.T) {
	v := startHTTP(t, "http_server", "")
	c := browse(t, v, 42001)
	if resp := c.fetch("HEAD /h HTTP/1.0\r\n\r\n"); resp != okHead+"8\r\nConnection: close\r\nContent-Type: text/plain\r\n\r\n" {
		t.Fatalf("HEAD: response %q", resp)
	}
	c = browse(t, v, 42002)
	if resp := c.fetch("POST / HTTP/1.0\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nab"); resp != badRequest {
		t.Fatalf("two lengths: response %q", resp)
	}
	c = browse(t, v, 42003)
	if resp := c.fetch("GET /a\x01b HTTP/1.0\r\n\r\n"); resp != badRequest {
		t.Fatalf("control byte: response %q", resp)
	}
	c = browse(t, v, 42004)
	if resp := c.fetch("GET /badheader HTTP/1.0\r\n\r\n"); resp != "HTTP/1.0 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n" {
		t.Fatalf("bad header: response %q", resp)
	}
	stop(t, v, "start true\nexit 69\nreq HEAD /h 0  0 0\nreq GET /badheader 0  0 0\n")
}

// serveBody answers c's request with a body of n bytes (Content-Length n)
// in 1400-byte segments until the VM closes (FIN, completed) or resets
// (truncated); it returns the VM's last answer by peer port.
func (c *tcpConn) serveBody(n int) map[uint16][]sim.TCP {
	c.t.Helper()
	m := c.exchange(sim.PSH|sim.ACK, "HTTP/1.0 200 OK\r\nContent-Length: "+strconv.Itoa(n)+"\r\n\r\n")
	for off := 0; off < n; off += 1400 {
		k := n - off
		if k > 1400 {
			k = 1400
		}
		m = split(c.send(sim.PSH|sim.ACK, pattern(off, k)))
		s := m[c.pport]
		if len(s) > 0 && s[len(s)-1].Flags&sim.RST != 0 {
			c.rst(s)
			return m
		}
	}
	c.finish(m[c.pport])
	return m
}

// A caller with its own heap and the MCU default limit (16 KB) is killed
// by an 8 KB response body (MailboxOverflow: the body is charged to it);
// with Do's small maxBody, or a larger limit, it gets the body.
func TestHTTPCallerMemory(t *testing.T) {
	v := startHTTP(t, "http_caller", "")
	m := split(newConn(t, v, 0, 0).segs(v.Advance(100)))
	for _, port := range []uint16{80, 81, 82} {
		c, _ := dialed(t, v, gwHost, port, m[port])
		m = c.serveBody(8192)
		if port < 82 && len(m[port+1]) == 0 {
			m = split(c.segs(v.Sync()))
		}
	}
	stop(t, v, "start true\nexit 80 3\ngot true 2048 true 1499\nexit 81 0\ngot true 8192 false 1499\nexit 82 0\ndone\n")
}
