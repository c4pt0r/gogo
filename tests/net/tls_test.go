package nettest

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/rsa"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/binary"
	"encoding/pem"
	"fmt"
	"io"
	"math/big"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	"gogotools/tests/net/sim"
)

// The "tls" port against real TLS servers (Go crypto/tls on loopback),
// bridged through the simulator: the simulator plays the TCP peer at
// 10.0.0.1:443 (and any other port given a server) for the VM and relays
// the byte stream to and from a real socket; it answers DNS queries for
// the names it knows with 10.0.0.1. Certificates come from a test CA made here; the VM trusts it
// through $GOGO_TLS_CA_FILE. $GOGO_TLS_STATS makes the VM report its TLS
// counters and heap at exit (tls-stats).

// ---- certificates ----

type testPKI struct {
	caFile string
	ca     *x509.Certificate
	caKey  *ecdsa.PrivateKey
}

var (
	pkiOnce sync.Once
	pkiVal  *testPKI
	pkiErr  error
)

func newSerial() *big.Int {
	n, _ := rand.Int(rand.Reader, big.NewInt(1<<62))
	return n
}

func makeCA(name string) (*x509.Certificate, *ecdsa.PrivateKey, error) {
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return nil, nil, err
	}
	tmpl := &x509.Certificate{
		SerialNumber:          newSerial(),
		Subject:               pkix.Name{CommonName: name},
		NotBefore:             time.Now().Add(-48 * time.Hour),
		NotAfter:              time.Now().Add(10 * 365 * 24 * time.Hour),
		IsCA:                  true,
		BasicConstraintsValid: true,
		KeyUsage:              x509.KeyUsageCertSign | x509.KeyUsageDigitalSignature,
	}
	der, err := x509.CreateCertificate(rand.Reader, tmpl, tmpl, &key.PublicKey, key)
	if err != nil {
		return nil, nil, err
	}
	cert, err := x509.ParseCertificate(der)
	return cert, key, err
}

func pki(t *testing.T) *testPKI {
	t.Helper()
	pkiOnce.Do(func() {
		ca, key, err := makeCA("gogo test CA")
		if err != nil {
			pkiErr = err
			return
		}
		dir, err := os.MkdirTemp("", "gogo-tls-ca-")
		if err != nil {
			pkiErr = err
			return
		}
		f := filepath.Join(dir, "ca.pem")
		pkiErr = os.WriteFile(f, pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: ca.Raw}), 0o644)
		pkiVal = &testPKI{caFile: f, ca: ca, caKey: key}
	})
	if pkiErr != nil {
		t.Fatal(pkiErr)
	}
	return pkiVal
}

type certOpts struct {
	host       string
	rsa        bool
	notBefore  time.Time
	notAfter   time.Time
	otherIssue bool // signed by a CA the VM does not trust
}

func (p *testPKI) leaf(t *testing.T, o certOpts) tls.Certificate {
	t.Helper()
	if o.notBefore.IsZero() {
		o.notBefore = time.Now().Add(-24 * time.Hour)
	}
	if o.notAfter.IsZero() {
		o.notAfter = time.Now().Add(24 * time.Hour)
	}
	var pub, priv any
	if o.rsa {
		k, err := rsa.GenerateKey(rand.Reader, 2048)
		if err != nil {
			t.Fatal(err)
		}
		pub, priv = &k.PublicKey, k
	} else {
		k, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
		if err != nil {
			t.Fatal(err)
		}
		pub, priv = &k.PublicKey, k
	}
	issuer, issuerKey := p.ca, p.caKey
	if o.otherIssue {
		var err error
		issuer, issuerKey, err = makeCA("gogo test CA")
		if err != nil {
			t.Fatal(err)
		}
	}
	tmpl := &x509.Certificate{
		SerialNumber: newSerial(),
		Subject:      pkix.Name{CommonName: o.host},
		DNSNames:     []string{o.host},
		NotBefore:    o.notBefore,
		NotAfter:     o.notAfter,
		KeyUsage:     x509.KeyUsageDigitalSignature | x509.KeyUsageKeyEncipherment,
		ExtKeyUsage:  []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
	}
	der, err := x509.CreateCertificate(rand.Reader, tmpl, issuer, pub, issuerKey)
	if err != nil {
		t.Fatal(err)
	}
	return tls.Certificate{Certificate: [][]byte{der}, PrivateKey: priv}
}

// ---- the server side ----

// tlsServer serves every accepted loopback connection with handle (in its
// own goroutine); raw handlers get the plain TCP connection (no TLS).
type tlsServer struct {
	ln   net.Listener
	wg   sync.WaitGroup
	mu   sync.Mutex
	logs []string
}

func (s *tlsServer) logf(format string, a ...any) {
	s.mu.Lock()
	s.logs = append(s.logs, fmt.Sprintf(format, a...))
	s.mu.Unlock()
}

func startServer(t *testing.T, cert *tls.Certificate, handle func(s *tlsServer, c net.Conn)) *tlsServer {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	s := &tlsServer{ln: ln}
	go func() {
		for {
			c, err := ln.Accept()
			if err != nil {
				return
			}
			var conn net.Conn = c
			if cert != nil {
				conn = tls.Server(c, &tls.Config{Certificates: []tls.Certificate{*cert}, MinVersion: tls.VersionTLS12})
			}
			s.wg.Add(1)
			go func() {
				defer s.wg.Done()
				defer conn.Close()
				handle(s, conn)
			}()
		}
	}()
	t.Cleanup(func() {
		ln.Close()
		s.wg.Wait()
		if t.Failed() {
			t.Logf("server log: %v", s.logs)
		}
	})
	return s
}

// echo answers every read with the same bytes until EOF, then closes.
func echoHandler(s *tlsServer, c net.Conn) {
	b := make([]byte, 4096)
	for {
		n, err := c.Read(b)
		if n > 0 {
			if _, err := c.Write(b[:n]); err != nil {
				return
			}
		}
		if err != nil {
			s.logf("echo: %v", err)
			return
		}
	}
}

// ---- the bridge ----

// bconn is the simulator's end of one TCP connection to 10.0.0.1 (port
// 443 or another with a server), relayed to a loopback socket of the
// server.
type bconn struct {
	*tcpConn
	srv      net.Conn
	data     chan []byte // from the server; closed on EOF or error
	out      []byte      // from the server, not yet sent to the VM
	srvEOF   bool
	inflight []byte // sent to the VM, not acknowledged
	sentAt   uint64 // simulated time inflight was (re)sent
	finSent  bool
	vmFin    bool
	done     bool // reset either way, or both FINs exchanged
	needAck  bool
	resets   int // RSTs from the VM
	fwd      int    // payload bytes from the VM passed to the server
	fwdAt    uint64 // simulated time a segment was last passed on
}

type bridge struct {
	t      *testing.T
	v      *sim.VM
	addrs  map[uint16]string // server address by port of 10.0.0.1
	names  map[string]bool   // DNS names that resolve to 10.0.0.1
	conns  map[uint16]*bconn
	queue  [][]byte // frames from the VM not handled yet
	all    []*bconn
	frames int
	// stall, by server port: after this many payload bytes from the VM,
	// the link trickles: a segment is passed on (and acknowledged) at most
	// every 2 s of simulated time, the others dropped unacknowledged (each
	// write still completes within a wait, but the transfer barely moves).
	stall map[uint16]int
}

func newBridge(t *testing.T, v *sim.VM, addr string) *bridge {
	return &bridge{t: t, v: v, addrs: map[uint16]string{443: addr}, names: map[string]bool{},
		conns: map[uint16]*bconn{}, stall: map[uint16]int{}}
}

// exchange sends a frame and queues the VM's answer; false once the VM
// has exited.
func (b *bridge) exchange(f []byte) bool {
	fr, err := b.v.Exchange(f)
	b.queue = append(b.queue, fr...)
	if err == io.EOF {
		return false
	}
	if err != nil {
		b.t.Fatal(err)
	}
	return true
}

func (b *bridge) advance(ms uint32) bool {
	fr, err := b.v.Advance(ms)
	b.queue = append(b.queue, fr...)
	if err == io.EOF {
		return false
	}
	if err != nil {
		b.t.Fatal(err)
	}
	return true
}

func (b *bridge) open(syn sim.TCP) {
	c := &bconn{tcpConn: newConn(b.t, b.v, syn.DstPort, 0)}
	c.accept([]sim.TCP{syn})
	srv, err := net.Dial("tcp", b.addrs[syn.DstPort])
	if err != nil {
		b.t.Fatal(err)
	}
	c.srv = srv
	c.data = make(chan []byte, 64)
	go func() {
		defer close(c.data)
		for {
			buf := make([]byte, 16384)
			n, err := srv.Read(buf)
			if n > 0 {
				c.data <- buf[:n]
			}
			if err != nil {
				return
			}
		}
	}()
	b.conns[c.vport] = c
	b.all = append(b.all, c)
	f := c.frame(sim.SYN|sim.ACK, nil, sim.MSSOption(1460)...)
	c.seq++
	b.exchange(f)
}

func (c *bconn) close() {
	c.done = true
	if c.srv != nil {
		c.srv.Close()
	}
}

// handle takes one frame from the VM.
func (b *bridge) handle(f []byte) {
	b.frames++
	p := decode(b.t, f)
	if p.ARP != nil {
		if p.ARP.Op == sim.ARPRequest && p.ARP.TPA == gwHost.IP {
			b.exchange(arpReply(gwHost, vmHost))
		}
		return
	}
	if p.UDP != nil && p.UDP.DstPort == sim.DNSPort && p.IP.Dst == gwHost.IP {
		q, err := sim.ParseDNS(p.UDP.Payload)
		if err != nil || len(q.Questions) != 1 {
			b.t.Fatalf("bad DNS query from the VM: %v %+v", err, q)
		}
		name := q.Questions[0].Name
		a := sim.DNSAnswer(q, true)
		if b.names[name] {
			a = sim.DNSAnswer(q, false, sim.ARecord(name, gwHost.IP, 60))
		}
		b.exchange(dnsFrame(gwHost, p.UDP.SrcPort, a.Bytes()))
		return
	}
	if p.TCP == nil {
		b.t.Fatalf("unexpected frame from the VM: %+v", p)
	}
	s := *p.TCP
	c := b.conns[s.SrcPort]
	if c == nil || c.done {
		if s.Flags == sim.SYN && b.addrs[s.DstPort] != "" {
			b.open(s)
		}
		return // late segments of a finished connection
	}
	if s.Flags&sim.RST != 0 {
		c.resets++
		c.close()
		return
	}
	if s.Flags&sim.ACK != 0 && c.inflight != nil && s.Ack == c.seq {
		c.inflight = nil
	}
	if len(s.Payload) > 0 {
		if lim, ok := b.stall[c.pport]; ok && c.fwd >= lim && b.v.Now()-c.fwdAt < 2000 {
			return // trickling: neither passed on nor acknowledged
		}
		if s.Seq == c.ack {
			c.fwd += len(s.Payload)
			c.fwdAt = b.v.Now()
			if c.srvEOF && c.finSent {
				// The server is gone: answer with a reset (as a real host does).
				f := c.frame(sim.RST|sim.ACK, nil)
				c.close()
				b.exchange(f)
				return
			}
			c.ack += uint32(len(s.Payload))
			if _, err := c.srv.Write(s.Payload); err != nil {
				c.srvEOF = true
			}
		}
		c.needAck = true
	}
	if s.Flags&sim.FIN != 0 && s.Seq+uint32(len(s.Payload)) == c.ack {
		c.ack++
		c.vmFin = true
		c.needAck = true
		if tc, ok := c.srv.(*net.TCPConn); ok {
			tc.CloseWrite()
		}
	}
}

// step moves data one hop each way; false when nothing happened.
func (b *bridge) step() bool {
	progressed := false
	for len(b.queue) > 0 {
		f := b.queue[0]
		b.queue = b.queue[1:]
		b.handle(f)
		progressed = true
	}
	for _, c := range b.all {
		if c.done {
			continue
		}
		for more := true; more; {
			select {
			case d, ok := <-c.data:
				if !ok {
					c.srvEOF = true
					c.data = nil
					more = false
				} else {
					c.out = append(c.out, d...)
				}
			default:
				more = false
			}
			if c.data == nil {
				break
			}
		}
		switch {
		case c.inflight == nil && len(c.out) > 0:
			n := len(c.out)
			if n > 1460 {
				n = 1460
			}
			c.inflight = c.out[:n:n]
			c.out = c.out[n:]
			c.sentAt = b.v.Now()
			f := c.frame(sim.PSH|sim.ACK, c.inflight)
			c.seq += uint32(n)
			c.needAck = false
			b.exchange(f)
			progressed = true
		case c.inflight != nil && b.v.Now()-c.sentAt >= 1000:
			// Not acknowledged (the VM refused it): resend.
			c.seq -= uint32(len(c.inflight))
			f := c.frame(sim.PSH|sim.ACK, c.inflight)
			c.seq += uint32(len(c.inflight))
			c.sentAt = b.v.Now()
			c.needAck = false
			b.exchange(f)
			progressed = true
		case c.inflight == nil && c.srvEOF && !c.finSent:
			f := c.frame(sim.FIN|sim.ACK, nil)
			c.seq++
			c.finSent = true
			c.needAck = false
			b.exchange(f)
			progressed = true
		case c.needAck:
			c.needAck = false
			b.exchange(c.frame(sim.ACK, nil))
			progressed = true
		}
		if c.finSent && c.vmFin && c.inflight == nil {
			c.close()
		}
	}
	return progressed
}

// wait for data from any live server connection, up to d of real time.
func (b *bridge) wait(d time.Duration) {
	deadline := time.Now().Add(d)
	for time.Now().Before(deadline) {
		for _, c := range b.all {
			if !c.done && c.data != nil && len(c.data) > 0 {
				return
			}
		}
		time.Sleep(200 * time.Microsecond)
	}
}

// run relays until the VM exits (real-time limit: 25 s).
func (b *bridge) run() {
	b.t.Helper()
	limit := time.Now().Add(25 * time.Second)
	if !b.advance(0) {
		b.drainQueue()
		return
	}
	idle := 0
	for !b.v.Closed() {
		if time.Now().After(limit) {
			b.t.Fatalf("bridge: the VM did not finish in time (sim time %d)", b.v.Now())
		}
		if b.step() {
			idle = 0
			continue
		}
		idle++
		if idle < 3 {
			b.wait(5 * time.Millisecond) // the server may be computing
			continue
		}
		if !b.advance(50) {
			break
		}
	}
	b.drainQueue()
}

func (b *bridge) drainQueue() {
	for len(b.queue) > 0 {
		f := b.queue[0]
		b.queue = b.queue[1:]
		p := decode(b.t, f)
		if p.TCP != nil {
			if c := b.conns[p.TCP.SrcPort]; c != nil && p.TCP.Flags&sim.RST != 0 {
				c.resets++
			}
		}
	}
	for _, c := range b.all {
		c.close()
	}
}

// ---- the VM side ----

var unixNow = time.Now().Unix()

type tlsRun struct {
	v     *sim.VM
	b     *bridge
	stats map[string]int64
}

// needTLS skips when $GOGO_VM was built without the "tls" port.
func needTLS(t *testing.T) {
	t.Helper()
	vm := os.Getenv("GOGO_VM")
	if vm == "" {
		t.Skip("GOGO_VM not set")
	}
	if err := exec.Command(vm, "--has-tls").Run(); err != nil {
		t.Skip("gogovm built without the tls port (no mbedTLS source)")
	}
}

// runTLS starts progs/tls_client with script (and extra config lines), the
// test CA trusted, relays it to srv until it exits, and checks its stdout
// (unless want is empty: then the caller does).
func runTLS(t *testing.T, srv *tlsServer, script, extra, want string) *tlsRun {
	t.Helper()
	needTLS(t)
	p := pki(t)
	// extra first: the first line for a key wins.
	cfg := extra + "test.script=" + script + "\ntest.time=" + strconv.FormatInt(unixNow, 10) + "\ntest.host=gogo.test\n"
	path := filepath.Join(t.TempDir(), "config")
	if err := os.WriteFile(path, []byte(cfg), 0o600); err != nil {
		t.Fatal(err)
	}
	t.Setenv("GOGO_CONFIG", path)
	t.Setenv("GOGO_TLS_CA_FILE", p.caFile)
	t.Setenv("GOGO_TLS_STATS", "1")
	v := start(t, "tls_client")
	addr := ""
	if srv != nil {
		addr = srv.ln.Addr().String()
	}
	b := newBridge(t, v, addr)
	b.run()
	if want != "" {
		finish(t, v, want)
	} else if err := v.Wait(); err != nil || !v.Closed() { // the caller checks stdout itself
		t.Errorf("VM: %v (want exit status 0, link closed)", err)
	}
	r := &tlsRun{v: v, b: b, stats: parseStats(t, v.Stderr.String())}
	// Every connection gives all its memory back: the TLS heap is back to
	// what tls_init left (PSA's global state, set up once at boot), plus
	// PSA's first volatile key-slot slice (16 slots, under 1 KB), made by
	// the first connection and kept (the same after two: TestTLSReconnect).
	if d := r.stats["heap_now"] - r.stats["heap_base"]; d < 0 || d > 1024 || r.stats["live"] != 0 {
		t.Errorf("TLS memory not freed at exit: %v", r.stats)
	}
	return r
}

var statsRE = regexp.MustCompile(`tls-stats (.*)`)

func parseStats(t *testing.T, stderr string) map[string]int64 {
	t.Helper()
	m := statsRE.FindStringSubmatch(stderr)
	if m == nil {
		t.Fatalf("no tls-stats line in stderr:\n%s", stderr)
	}
	out := map[string]int64{}
	for _, kv := range strings.Fields(m[1]) {
		k, v, _ := strings.Cut(kv, "=")
		n, err := strconv.ParseInt(v, 10, 64)
		if err != nil {
			t.Fatalf("tls-stats %q: %v", kv, err)
		}
		out[k] = n
	}
	return out
}

// ---- tests ----

// Before the wall clock is set a Dial gets Closed{NoTime}: nothing is sent.
func TestTLSNoTime(t *testing.T) {
	r := runTLS(t, nil, "dial", "", "dial true\nclosed 6\ndone\n")
	if r.stats["opens"] != 0 || len(r.b.all) != 0 {
		t.Errorf("a connection was made: %v", r.stats)
	}
}

// An ECDSA server: handshake (ECDHE-ECDSA, the signature checks sliced by
// restartable ECC), echo, Shutdown (close_notify; the server closes), and
// a second open while one is up is refused (GOGO_TLS_MAX 1).
func TestTLSEchoECDSA(t *testing.T) {
	cert := pki(t).leaf(t, certOpts{host: "gogo.test"})
	var curve tls.CurveID
	var mu sync.Mutex
	srv := startServer(t, &cert, func(s *tlsServer, c net.Conn) {
		if tc, ok := c.(*tls.Conn); ok && tc.Handshake() == nil {
			mu.Lock()
			curve = tc.ConnectionState().CurveID
			mu.Unlock()
		}
		echoHandler(s, c)
	})
	defer func() {
		mu.Lock()
		defer mu.Unlock()
		if curve != tls.X25519 { // tls.c's first group, as on the board
			t.Errorf("ECDHE group %v, want X25519", curve)
		}
	}()
	r := runTLS(t, srv, "time sleep:100 dial dial2 send:hello echo:5 send:world echo:5 shut close", "",
		"time true\ndial true\nconnected\ndial2 false\nwritten\necho hello\nwritten\necho world\nclosed 0\nclosed port\ndone\n")
	s := r.stats
	if s["handshakes"] != 1 {
		t.Errorf("handshakes %d", s["handshakes"])
	}
	// The ECDSA checks (ServerKeyExchange, the chain) gave up and resumed:
	// ECC is sliced.
	if s["restarts"] < 2 {
		t.Errorf("restarts %d: restartable ECC not used", s["restarts"])
	}
	t.Logf("ECDSA handshake: %d slices (%d restarts), longest %d us (state %d), longest read/write %d us, TLS heap peak %d B",
		s["steps"], s["restarts"], s["max_step_us"], s["max_step_state"], s["max_io_us"], s["heap_peak"])
	if c := r.b.all[0]; c.resets != 0 {
		t.Errorf("the VM reset a cleanly shut down connection (%d RST)", c.resets)
	}
}

// The sliced handshake never waits for an unrelated wakeup between its
// slices: a slice that gave up (or a step with more to do) leaves the net
// source pending, and the scheduler drains a pending source instead of
// sleeping until a frame or a timer (vm_run; also vm/test port_source_busy). The simulator advances its clock (50 ms a step)
// only while the VM is idle, so a stall shows as simulated time; without
// the fix every ECDSA restart cost a step.
func TestTLSHandshakeNoIdleStall(t *testing.T) {
	cert := pki(t).leaf(t, certOpts{host: "gogo.test"})
	srv := startServer(t, &cert, echoHandler)
	r := runTLS(t, srv, "time sleep:100 dialtime close", "", "")
	out := r.v.Stdout.String()
	m := regexp.MustCompile(`handshake sim ms (\d+)`).FindStringSubmatch(out)
	if m == nil {
		t.Fatalf("no handshake time in %q", out)
	}
	ms, _ := strconv.Atoi(m[1])
	if !strings.HasPrefix(out, "time true\ndial true\nconnected\n") || !strings.HasSuffix(out, "closed port\ndone\n") {
		t.Errorf("stdout %q", out)
	}
	if r.stats["restarts"] < 2 || ms >= 50 {
		t.Errorf("handshake took %d ms of simulated time with %d restarts: it idled between slices", ms,
			r.stats["restarts"])
	}
}

// ---- the ESP trust-anchor rule (runtime/net/tls/anchor.h) ----

// espBundle writes certs (subject DER, SubjectPublicKeyInfo DER) in
// ESP-IDF's esp_crt_bundle format: a u32 LE offset per certificate (the
// first is the table's size), then per certificate u16 LE subject and key
// lengths, the subject and the key; sorted by subject.
func espBundle(t *testing.T, entries [][2][]byte) string {
	sort.Slice(entries, func(i, j int) bool { return string(entries[i][0]) < string(entries[j][0]) })
	var body []byte
	offs := make([]byte, 4*len(entries))
	for i, e := range entries {
		binary.LittleEndian.PutUint32(offs[4*i:], uint32(len(offs)+len(body)))
		var h [4]byte
		binary.LittleEndian.PutUint16(h[0:], uint16(len(e[0])))
		binary.LittleEndian.PutUint16(h[2:], uint16(len(e[1])))
		body = append(append(append(body, h[:]...), e[0]...), e[1]...)
	}
	path := filepath.Join(t.TempDir(), "bundle.bin")
	if err := os.WriteFile(path, append(offs, body...), 0o644); err != nil {
		t.Fatal(err)
	}
	return path
}

// crossChain is a chain whose last certificate is a root cross-signed by
// another CA (as example.com's): leaf <- intermediate <- "gogo bundle
// root" (key rootKey) signed by "gogo old root", which no bundle holds.
type crossChain struct {
	cert        tls.Certificate // served: leaf, intermediate, cross-signed root
	rootSubject []byte
	rootSPKI    []byte
	otherSPKI   []byte // a key of no certificate in the chain
	otherCASubj []byte // the old root's subject
	otherCASPKI []byte
}

func makeCrossChain(t *testing.T) *crossChain {
	return makeCrossChainAt(t, time.Now().Add(365*24*time.Hour))
}

// makeCrossChainAt: the cross-signed root's NotAfter is rootNotAfter.
func makeCrossChainAt(t *testing.T, rootNotAfter time.Time) *crossChain {
	t.Helper()
	old, oldKey, err := makeCA("gogo old root")
	if err != nil {
		t.Fatal(err)
	}
	gen := func() *ecdsa.PrivateKey {
		k, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
		if err != nil {
			t.Fatal(err)
		}
		return k
	}
	create := func(tmpl, parent *x509.Certificate, pub, signer any) *x509.Certificate {
		der, err := x509.CreateCertificate(rand.Reader, tmpl, parent, pub, signer)
		if err != nil {
			t.Fatal(err)
		}
		c, err := x509.ParseCertificate(der)
		if err != nil {
			t.Fatal(err)
		}
		return c
	}
	ca := func(name string) *x509.Certificate {
		return &x509.Certificate{SerialNumber: newSerial(), Subject: pkix.Name{CommonName: name},
			NotBefore: time.Now().Add(-48 * time.Hour), NotAfter: time.Now().Add(365 * 24 * time.Hour),
			IsCA: true, BasicConstraintsValid: true, KeyUsage: x509.KeyUsageCertSign | x509.KeyUsageDigitalSignature}
	}
	rootKey, intKey, leafKey, other := gen(), gen(), gen(), gen()
	rootTmpl := ca("gogo bundle root")
	rootTmpl.NotAfter = rootNotAfter
	if rootNotAfter.Before(time.Now()) {
		rootTmpl.NotBefore = rootNotAfter.Add(-48 * time.Hour)
	}
	cross := create(rootTmpl, old, &rootKey.PublicKey, oldKey) // the root's key, signed by the old root
	inter := create(ca("gogo intermediate"), cross, &intKey.PublicKey, rootKey)
	leaf := create(&x509.Certificate{SerialNumber: newSerial(), Subject: pkix.Name{CommonName: "gogo.test"},
		DNSNames: []string{"gogo.test"}, NotBefore: time.Now().Add(-24 * time.Hour), NotAfter: time.Now().Add(24 * time.Hour),
		KeyUsage: x509.KeyUsageDigitalSignature, ExtKeyUsage: []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth}},
		inter, &leafKey.PublicKey, intKey)
	otherSPKI, err := x509.MarshalPKIXPublicKey(&other.PublicKey)
	if err != nil {
		t.Fatal(err)
	}
	return &crossChain{
		cert:        tls.Certificate{Certificate: [][]byte{leaf.Raw, inter.Raw, cross.Raw}, PrivateKey: leafKey},
		rootSubject: cross.RawSubject, rootSPKI: cross.RawSubjectPublicKeyInfo, otherSPKI: otherSPKI,
		otherCASubj: old.RawSubject, otherCASPKI: old.RawSubjectPublicKeyInfo,
	}
}

// The ESP rule on the host (GOGO_TLS_BUNDLE_FILE): the last certificate
// sent is trusted only if a bundle entry has its subject AND its key;
// whatever signed it does not matter. Negatives: the right subject with
// another key, the right key under another subject, the right entry cut
// short (a corrupt bundle), and a bundle with only the cross-signer's root
// (the rule never checks the presented root's signature).
func TestTLSBundleAnchor(t *testing.T) {
	cc := makeCrossChain(t)
	srv := startServer(t, &cc.cert, echoHandler)
	right := [2][]byte{cc.rootSubject, cc.rootSPKI}
	filler := [2][]byte{[]byte("\x30\x00"), cc.otherSPKI}
	cases := []struct {
		name    string
		entries [][2][]byte
		cut     int
		want    string
	}{
		{"subject and key", [][2][]byte{filler, right}, 0, "connected"},
		{"subject, other key", [][2][]byte{{cc.rootSubject, cc.otherSPKI}}, 0, "closed 7"},
		{"key, other subject", [][2][]byte{{cc.otherCASubj, cc.rootSPKI}}, 0, "closed 7"},
		{"entry cut short", [][2][]byte{right}, 1, "closed 7"},
		{"only the cross-signer", [][2][]byte{{cc.otherCASubj, cc.otherCASPKI}}, 0, "closed 7"},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			path := espBundle(t, c.entries)
			if c.cut > 0 {
				b, _ := os.ReadFile(path)
				os.WriteFile(path, b[:len(b)-c.cut], 0o644)
			}
			t.Setenv("GOGO_TLS_BUNDLE_FILE", path)
			r := runTLS(t, srv, "time sleep:100 dial close", "", "time true\ndial true\n"+c.want+"\nclosed port\ndone\n")
			if c.want == "connected" && r.stats["handshakes"] != 1 {
				t.Errorf("handshakes %d", r.stats["handshakes"])
			}
		})
	}
}

// The rule's flag cases: a presented root that matches the bundle but has
// expired is refused (any flag besides NOT_TRUSTED fails it; stricter than
// browsers, followup 17), and so is a lone leaf that is in the bundle
// itself but is not for the name dialed (NOT_TRUSTED | CN_MISMATCH).
func TestTLSBundleAnchorFlags(t *testing.T) {
	t.Run("expired cross-signed root", func(t *testing.T) {
		cc := makeCrossChainAt(t, time.Now().Add(-24*time.Hour))
		srv := startServer(t, &cc.cert, echoHandler)
		t.Setenv("GOGO_TLS_BUNDLE_FILE", espBundle(t, [][2][]byte{{cc.rootSubject, cc.rootSPKI}}))
		runTLS(t, srv, "time sleep:100 dial close", "", "time true\ndial true\nclosed 7\nclosed port\ndone\n")
	})
	t.Run("leaf in bundle, wrong name", func(t *testing.T) {
		k, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
		if err != nil {
			t.Fatal(err)
		}
		tmpl := &x509.Certificate{SerialNumber: newSerial(), Subject: pkix.Name{CommonName: "other.test"},
			DNSNames: []string{"other.test"}, NotBefore: time.Now().Add(-24 * time.Hour), NotAfter: time.Now().Add(24 * time.Hour),
			KeyUsage: x509.KeyUsageDigitalSignature, ExtKeyUsage: []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth}}
		der, err := x509.CreateCertificate(rand.Reader, tmpl, tmpl, &k.PublicKey, k)
		if err != nil {
			t.Fatal(err)
		}
		leaf, _ := x509.ParseCertificate(der)
		cert := tls.Certificate{Certificate: [][]byte{der}, PrivateKey: k}
		srv := startServer(t, &cert, echoHandler)
		t.Setenv("GOGO_TLS_BUNDLE_FILE", espBundle(t, [][2][]byte{{leaf.RawSubject, leaf.RawSubjectPublicKeyInfo}}))
		runTLS(t, srv, "time sleep:100 dial close", "", "time true\ndial true\nclosed 7\nclosed port\ndone\n")
	})
}

// An RSA server: ECDHE-RSA works too.
func TestTLSEchoRSA(t *testing.T) {
	cert := pki(t).leaf(t, certOpts{host: "gogo.test", rsa: true})
	srv := startServer(t, &cert, echoHandler)
	r := runTLS(t, srv, "time sleep:100 dial send:rsa echo:3 shut close", "",
		"time true\ndial true\nconnected\nwritten\necho rsa\nclosed 0\nclosed port\ndone\n")
	t.Logf("RSA handshake: %d slices, longest %d us (state %d)", r.stats["steps"], r.stats["max_step_us"],
		r.stats["max_step_state"])
}

// Certificates the VM must refuse: expired, for another host, from an
// untrusted CA, not yet valid. Each is Closed{BadCert}, the connection
// reset, the memory freed.
func TestTLSBadCert(t *testing.T) {
	p := pki(t)
	cases := []struct {
		name  string
		opts  certOpts
		extra string
	}{
		{"expired", certOpts{host: "gogo.test", notBefore: time.Now().Add(-72 * time.Hour), notAfter: time.Now().Add(-24 * time.Hour)}, ""},
		{"wrong host", certOpts{host: "gogo.test"}, "test.host=other.test\n"},
		{"untrusted", certOpts{host: "gogo.test", otherIssue: true}, ""},
		{"not yet valid", certOpts{host: "gogo.test", notBefore: time.Now().Add(24 * time.Hour), notAfter: time.Now().Add(72 * time.Hour)}, ""},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			cert := p.leaf(t, tc.opts)
			srv := startServer(t, &cert, echoHandler)
			r := runTLS(t, srv, "time sleep:100 dial close", tc.extra,
				"time true\ndial true\nclosed 7\nclosed port\ndone\n")
			if r.stats["handshakes"] != 0 {
				t.Errorf("handshake completed: %v", r.stats)
			}
		})
	}
}

// The peer closes in the middle of the handshake (after reading the
// ClientHello): a clean Closed{Reset}, no leak.
func TestTLSPeerClosesMidHandshake(t *testing.T) {
	srv := startServer(t, nil, func(s *tlsServer, c net.Conn) {
		b := make([]byte, 64)
		n, err := io.ReadAtLeast(c, b, 5)
		s.logf("read %d %v", n, err)
	})
	runTLS(t, srv, "time sleep:100 dial close", "", "time true\ndial true\nclosed 1\nclosed port\ndone\n")
}

// A server that accepts TCP but never answers: TimeoutMs bounds the
// handshake (Closed{Timeout}).
func TestTLSHandshakeTimeout(t *testing.T) {
	srv := startServer(t, nil, func(s *tlsServer, c net.Conn) { io.Copy(io.Discard, c) })
	runTLS(t, srv, "time sleep:100 dial close", "test.timeout=2000\n",
		"time true\ndial true\nclosed 2\nclosed port\ndone\n")
}

// The owner exits in the middle of the handshake (the server is slow to
// answer): the port closes with it, the connection is reset and every TLS
// buffer is freed (runTLS checks the heap).
func TestTLSOwnerExitsDuringHandshake(t *testing.T) {
	cert := pki(t).leaf(t, certOpts{host: "gogo.test"})
	srv := startServer(t, nil, func(s *tlsServer, c net.Conn) {
		time.Sleep(300 * time.Millisecond)
		tc := tls.Server(c, &tls.Config{Certificates: []tls.Certificate{cert}})
		s.logf("handshake: %v", tc.Handshake())
	})
	r := runTLS(t, srv, "time sleep:100 open sleep:100 exit", "", "time true\nopen true\nexit\ndone\n")
	if r.stats["opens"] != 1 || r.stats["handshakes"] != 0 {
		t.Errorf("stats %v", r.stats)
	}
	if len(r.b.all) != 1 || r.b.all[0].resets == 0 {
		t.Errorf("the connection was not reset")
	}
}

// 1 MB up through the port with a bounded heap: the server checks every
// byte and answers with the count; then a 256 KB download, also checked.
func TestTLSLargeTransfer(t *testing.T) {
	cert := pki(t).leaf(t, certOpts{host: "gogo.test"})
	const up, down = 1 << 20, 256 << 10
	srv := startServer(t, &cert, func(s *tlsServer, c net.Conn) {
		b := make([]byte, 16384)
		got, bad := 0, 0
		for got < up {
			n, err := c.Read(b)
			for i := 0; i < n; i++ {
				if b[i] != byte((got+i)%251) {
					bad++
				}
			}
			got += n
			if err != nil {
				s.logf("upload: %v after %d", err, got)
				return
			}
		}
		reply := "got " + strconv.Itoa(got) + " bad " + strconv.Itoa(bad)
		for len(reply) < 20 {
			reply += "."
		}
		c.Write([]byte(reply))
		p := make([]byte, down)
		for i := range p {
			p[i] = byte(i % 251)
		}
		c.Write(p)
	})
	r := runTLS(t, srv, "time sleep:100 dial upload:1048576 echo:20 recv:262144 recvall close", "",
		"time true\ndial true\nconnected\nuploaded 1048576\necho got 1048576 bad 0...\nrecv 262144 bad 0\nrecvall 0 closed 0\nclosed port\ndone\n")
	peak := r.stats["heap_peak"]
	t.Logf("1 MB up, 256 KB down: TLS heap peak %d B, longest read/write slice %d us", peak, r.stats["max_io_us"])
	if peak > 48<<10 {
		t.Errorf("TLS heap peak %d B: not bounded", peak)
	}
}

// Reuse after the server closed: the next Out gets Done{-1} and a clean Closed (the
// server's host resets it), and a new Dial works.
func TestTLSReconnect(t *testing.T) {
	cert := pki(t).leaf(t, certOpts{host: "gogo.test"})
	n := 0
	var mu sync.Mutex
	srv := startServer(t, &cert, func(s *tlsServer, c net.Conn) {
		mu.Lock()
		n++
		k := n
		mu.Unlock()
		b := make([]byte, 64)
		m, err := c.Read(b)
		if err != nil {
			return
		}
		c.Write(b[:m])
		if k == 1 {
			return // close_notify, then the FIN
		}
		echoHandler(s, c)
	})
	runTLS(t, srv, "time sleep:100 dial send:one echo:3 recvall sleep:100 send:again close dial send:two echo:3 shut close", "",
		"time true\ndial true\nconnected\nwritten\necho one\nrecvall 0 closed 0\nnot sent\nclosed 1\nclosed port\n"+
			"dial true\nconnected\nwritten\necho two\nclosed 0\nclosed port\ndone\n")
}

// An owner whose limit cannot hold the TLS state is refused at open.
func TestTLSOwnerTooSmall(t *testing.T) {
	runTLS(t, nil, "small", "", "small false\ndone\n")
}

// Without a secure random source (platform_tls_random fails, as its weak
// default does) there is no TLS: the open is refused.
func TestTLSNoEntropy(t *testing.T) {
	t.Setenv("GOGO_TLS_TEST_NO_RANDOM", "1")
	r := runTLS(t, nil, "time sleep:100 dial", "", "time true\ndial false\ndone\n")
	if r.stats["opens"] != 0 || len(r.b.all) != 0 {
		t.Errorf("a connection was made: %v", r.stats)
	}
}
