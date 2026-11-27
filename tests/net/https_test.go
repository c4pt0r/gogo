package nettest

import (
	"bufio"
	"crypto/tls"
	"fmt"
	"io"
	"net"
	nethttp "net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	"gogotools/tests/net/sim"
)

// gogo/http over the bridge (tls_test.go): https (the "tls" port), the
// keep-alive Client, Upload, and plain http through the same code, against
// Go's net/http server (or a raw handler for broken servers).

// httpSrv is a net/http server on loopback, TLS or plain, that counts its
// connections and requests.
type httpSrv struct {
	addr  string
	mu    sync.Mutex
	conns int
	reqs  []string // "METHOD path proto conn-header auth-header"
}

func (s *httpSrv) count() (int, []string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.conns, append([]string(nil), s.reqs...)
}

// startHTTPServer serves h (after logging the request); cert nil: plain
// http. idleClose closes every connection as soon as it is idle (a server
// dropping idle keep-alive connections).
func startHTTPServer(t *testing.T, cert *tls.Certificate, idleClose bool, h nethttp.HandlerFunc) *httpSrv {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	s := &httpSrv{addr: ln.Addr().String()}
	if cert != nil {
		ln = tls.NewListener(ln, &tls.Config{Certificates: []tls.Certificate{*cert}, MinVersion: tls.VersionTLS12})
	}
	srv := &nethttp.Server{
		Handler: nethttp.HandlerFunc(func(w nethttp.ResponseWriter, r *nethttp.Request) {
			s.mu.Lock()
			s.reqs = append(s.reqs, fmt.Sprintf("%s %s %s %s %q %q", r.Method, r.URL.Path, r.Proto, r.Host,
				r.Header.Get("Connection"), r.Header.Get("Authorization")))
			s.mu.Unlock()
			h(w, r)
		}),
		ConnState: func(c net.Conn, st nethttp.ConnState) {
			switch st {
			case nethttp.StateNew:
				s.mu.Lock()
				s.conns++
				s.mu.Unlock()
			case nethttp.StateIdle:
				if idleClose {
					c.Close()
				}
			}
		},
		ErrorLog: nil,
	}
	go srv.Serve(ln)
	t.Cleanup(func() { srv.Close() })
	return s
}

// hello answers "hello <path>" (with Content-Length: net/http sets it for
// small bodies), or echoes a POST body.
func hello(w nethttp.ResponseWriter, r *nethttp.Request) {
	if r.Method == "POST" {
		b, _ := io.ReadAll(r.Body)
		w.WriteHeader(201)
		w.Write(b)
		return
	}
	io.WriteString(w, "hello "+r.URL.Path)
}

type httpsRun struct {
	*tlsRun
	stdout string
}

// runHTTPS runs progs/https_client with script against the servers by
// port of 10.0.0.1 (443 TLS, 80 plain), the names gogo.test and other.test
// resolving to it, the test CA trusted and extra config lines; files are
// put under the VM's /flash first. It checks stdout and that the TLS
// memory came back.
func runHTTPS(t *testing.T, servers map[uint16]string, files map[string][]byte, script, extra, want string,
	vmArgs ...string) *httpsRun {
	t.Helper()
	return runHTTPSStall(t, servers, nil, files, script, extra, want, vmArgs...)
}

// runHTTPSStall is runHTTPS with the bridge stalling connections to the
// server ports in stall after so many bytes from the VM (bridge.stall).
func runHTTPSStall(t *testing.T, servers map[uint16]string, stall map[uint16]int, files map[string][]byte,
	script, extra, want string, vmArgs ...string) *httpsRun {
	t.Helper()
	needTLS(t)
	p := pki(t)
	root := t.TempDir()
	if err := os.MkdirAll(filepath.Join(root, "flash"), 0o755); err != nil {
		t.Fatal(err)
	}
	for name, data := range files {
		if err := os.WriteFile(filepath.Join(root, "flash", name), data, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	cfg := extra + "test.script=" + script +
		"\ntest.time=" + strconv.FormatInt(unixNow, 10) + "\n"
	path := filepath.Join(t.TempDir(), "config")
	if err := os.WriteFile(path, []byte(cfg), 0o600); err != nil {
		t.Fatal(err)
	}
	t.Setenv("GOGO_CONFIG", path)
	t.Setenv("GOGO_TLS_CA_FILE", p.caFile)
	t.Setenv("GOGO_TLS_STATS", "1")
	v := start(t, "https_client", append([]string{"--fs-root", root}, vmArgs...)...)
	b := newBridge(t, v, "")
	for port, addr := range servers {
		b.addrs[port] = addr
	}
	b.names["gogo.test"] = true
	b.names["other.test"] = true
	for port, n := range stall {
		b.stall[port] = n
	}
	b.run()
	finish(t, v, want)
	r := &tlsRun{v: v, b: b, stats: parseStats(t, v.Stderr.String())}
	if d := r.stats["heap_now"] - r.stats["heap_base"]; d < 0 || d > 1024 || r.stats["live"] != 0 {
		t.Errorf("TLS memory not freed at exit: %v", r.stats)
	}
	return &httpsRun{tlsRun: r, stdout: v.Stdout.String()}
}

func leafServer(t *testing.T, idleClose bool, h nethttp.HandlerFunc) *httpSrv {
	cert := pki(t).leaf(t, certOpts{host: "gogo.test"})
	return startHTTPServer(t, &cert, idleClose, h)
}

// A one-shot Get and Post over https: HTTP/1.1 with Connection: close
// (over http HTTP/1.0: TestHTTPPlainSamePath); the name resolved, the
// certificate checked for it (SNI); the connection closed. Before the
// clock is set, https fails.
func TestHTTPSGet(t *testing.T) {
	s := leafServer(t, false, hello)
	r := runHTTPS(t, map[uint16]string{443: s.addr}, nil,
		"GET:https://gogo.test/early time GET:https://gogo.test/a?x=1 POST:https://gogo.test:443/p", "test.body=posted\n",
		"start true\n"+
			"GET https://gogo.test/early false 0 0 false\n  body []\n"+
			"time true\n"+
			"GET https://gogo.test/a?x=1 true 200 8 false\n  body [hello /a]\n"+
			"POST https://gogo.test:443/p true 201 6 false\n  body [posted]\n"+
			"done\n")
	conns, reqs := s.count()
	if want := []string{`GET /a HTTP/1.1 gogo.test "close" ""`, `POST /p HTTP/1.1 gogo.test:443 "close" ""`}; conns != 2 || fmt.Sprint(reqs) != fmt.Sprint(want) {
		t.Errorf("server saw %d connections, %q; want 2, %q", conns, reqs, want)
	}
	// No reset check: after "Connection: close" the server's close and
	// the VM's close_notify cross, and the port may close before the
	// close_notify is acknowledged (a reset after the complete response).
	if r.stats["handshakes"] != 2 {
		t.Errorf("handshakes %d", r.stats["handshakes"])
	}
}

// A certificate for another name is refused: no handshake, no request.
func TestHTTPSWrongHost(t *testing.T) {
	s := leafServer(t, false, hello)
	r := runHTTPS(t, map[uint16]string{443: s.addr}, nil, "time GET:https://other.test/ new CGET:https://other.test/ cclose", "",
		"start true\ntime true\n"+
			"GET https://other.test/ false 0 0 false\n  body []\n"+
			"new\nCGET https://other.test/ false 0 0 false\n  body []\ncclose\n"+
			"done\n")
	if _, reqs := s.count(); len(reqs) != 0 || r.stats["handshakes"] != 0 {
		t.Errorf("requests %q, handshakes %d", reqs, r.stats["handshakes"])
	}
}

// The Client keeps one HTTP/1.1 connection: three requests (a POST among
// them) over one handshake; CloseClient ends it cleanly.
func TestHTTPSKeepAlive(t *testing.T) {
	s := leafServer(t, false, hello)
	r := runHTTPS(t, map[uint16]string{443: s.addr}, nil,
		"time new CGET:https://gogo.test/1 CPOST:https://gogo.test/2 sleep:500 CGET:https://gogo.test/3 cclose", "test.body=two\n",
		"start true\ntime true\nnew\n"+
			"CGET https://gogo.test/1 true 200 8 false\n  body [hello /1]\n"+
			"CPOST https://gogo.test/2 true 201 3 false\n  body [two]\n"+
			"CGET https://gogo.test/3 true 200 8 false\n  body [hello /3]\n"+
			"cclose\ndone\n")
	conns, reqs := s.count()
	if conns != 1 || r.stats["handshakes"] != 1 || len(reqs) != 3 {
		t.Errorf("connections %d, handshakes %d, requests %q", conns, r.stats["handshakes"], reqs)
	}
	if len(reqs) > 0 && reqs[0] != `GET /1 HTTP/1.1 gogo.test "" ""` {
		t.Errorf("request %q", reqs[0])
	}
	if c := r.b.all[0]; c.resets != 0 {
		t.Errorf("CloseClient reset the connection")
	}
}

// A server that drops idle connections: the Client notices the close
// while idle and reconnects for the next request (nothing was sent on
// the dead connection).
func TestHTTPSIdleClose(t *testing.T) {
	s := leafServer(t, true, hello)
	r := runHTTPS(t, map[uint16]string{443: s.addr}, nil,
		"time new CGET:https://gogo.test/1 sleep:1000 CGET:https://gogo.test/2 cclose", "",
		"start true\ntime true\nnew\n"+
			"CGET https://gogo.test/1 true 200 8 false\n  body [hello /1]\n"+
			"CGET https://gogo.test/2 true 200 8 false\n  body [hello /2]\n"+
			"cclose\ndone\n")
	if conns, reqs := s.count(); conns != 2 || len(reqs) != 2 || r.stats["handshakes"] != 2 {
		t.Errorf("connections %d, requests %q, handshakes %d", conns, reqs, r.stats["handshakes"])
	}
}

// rawTLS serves each connection with handle over TLS (no net/http), for
// servers that misbehave.
func rawTLS(t *testing.T, handle func(n int, c net.Conn, br *bufio.Reader)) *tlsServer {
	cert := pki(t).leaf(t, certOpts{host: "gogo.test"})
	var mu sync.Mutex
	n := 0
	return startServer(t, &cert, func(s *tlsServer, c net.Conn) {
		mu.Lock()
		n++
		k := n
		mu.Unlock()
		handle(k, c, bufio.NewReader(c))
	})
}

// readReq reads one request head (and a Content-Length body); "" at EOF.
func readReq(br *bufio.Reader) string {
	req, err := nethttp.ReadRequest(br)
	if err != nil {
		return ""
	}
	io.Copy(io.Discard, req.Body)
	return req.Method + " " + req.URL.Path
}

// A request whose bytes went out is never sent again: the server reads a
// POST on a kept connection and closes without answering; ClientDo fails
// and no second connection comes. The next request reconnects.
func TestHTTPSNoRetryAfterSend(t *testing.T) {
	var mu sync.Mutex
	var seen []string
	srv := rawTLS(t, func(n int, c net.Conn, br *bufio.Reader) {
		for {
			r := readReq(br)
			if r == "" {
				return
			}
			mu.Lock()
			seen = append(seen, strconv.Itoa(n)+" "+r)
			mu.Unlock()
			if r == "POST /drop" {
				return
			}
			io.WriteString(c, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok")
		}
	})
	runHTTPS(t, map[uint16]string{443: srv.ln.Addr().String()}, nil,
		"time new CGET:https://gogo.test/a CPOST:https://gogo.test/drop CGET:https://gogo.test/b cclose", "test.body=x\n",
		"start true\ntime true\nnew\n"+
			"CGET https://gogo.test/a true 200 2 false\n  body [ok]\n"+
			"CPOST https://gogo.test/drop false 0 0 false\n  body []\n"+
			"CGET https://gogo.test/b true 200 2 false\n  body [ok]\n"+
			"cclose\ndone\n")
	mu.Lock()
	defer mu.Unlock()
	if want := []string{"1 GET /a", "1 POST /drop", "2 GET /b"}; fmt.Sprint(seen) != fmt.Sprint(want) {
		t.Errorf("server saw %q, want %q", seen, want)
	}
}

// A response cut short by the peer's close at a record boundary (which
// TLS reports as a normal close) is an error, never a short success: a
// Content-Length body, a chunked body without its last chunk (one-shot
// and Client: both speak HTTP/1.1 over https), a body without a length
// (unframed over TLS). A whole chunked response is decoded.
func TestHTTPSTruncated(t *testing.T) {
	srv := rawTLS(t, func(n int, c net.Conn, br *bufio.Reader) {
		r := readReq(br)
		switch r {
		case "GET /len":
			io.WriteString(c, "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nshort")
		case "GET /chunked-cut":
			io.WriteString(c, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n")
		case "GET /nolen":
			io.WriteString(c, "HTTP/1.1 200 OK\r\n\r\nall of it?")
		case "GET /chunked":
			io.WriteString(c, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5;x=1\r\nhello\r\n7\r\n, world\r\n0\r\nX-T: 1\r\n\r\n")
			if readReq(br) == "GET /again" {
				io.WriteString(c, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nagain")
			}
		}
	})
	runHTTPS(t, map[uint16]string{443: srv.ln.Addr().String()}, nil,
		"time GET:https://gogo.test/len GET:https://gogo.test/nolen GET:https://gogo.test/chunked-cut new CGET:https://gogo.test/chunked-cut CGET:https://gogo.test/chunked CGET:https://gogo.test/again cclose", "",
		"start true\ntime true\n"+
			"GET https://gogo.test/len false 0 0 false\n  body []\n"+
			"GET https://gogo.test/nolen false 0 0 false\n  body []\n"+
			"GET https://gogo.test/chunked-cut false 0 0 false\n  body []\n"+
			"new\n"+
			"CGET https://gogo.test/chunked-cut false 0 0 false\n  body []\n"+
			"CGET https://gogo.test/chunked true 200 12 false\n  body [hello, world]\n"+
			"CGET https://gogo.test/again true 200 5 false\n  body [again]\n"+
			"cclose\ndone\n")
}

// A server that answers HTTP/1.0 with a body delimited only by its close
// (as Cloudflare does, example.com among its sites) and HTTP/1.1 with a
// chunked one: the one-shot https Get asks in HTTP/1.1, so the body is
// framed and accepted (an unframed one is refused over TLS).
func TestHTTPSGetFramedByVersion(t *testing.T) {
	srv := rawTLS(t, func(n int, c net.Conn, br *bufio.Reader) {
		req, err := nethttp.ReadRequest(br)
		if err != nil {
			return
		}
		if req.ProtoMinor == 0 {
			io.WriteString(c, "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nunframed")
		} else {
			io.WriteString(c, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n6\r\nframed\r\n0\r\n\r\n")
		}
		c.Close()
	})
	runHTTPS(t, map[uint16]string{443: srv.ln.Addr().String()}, nil, "time GET:https://gogo.test/", "",
		"start true\ntime true\nGET https://gogo.test/ true 200 6 false\n  body [framed]\ndone\n")
}

// upPattern is the upload test's file content.
func upPattern(n int) []byte {
	b := make([]byte, n)
	for i := range b {
		b[i] = byte(i % 251)
	}
	return b
}

// upServer checks an upload of size pattern bytes and answers 201.
func upServer(t *testing.T, size int) *httpSrv {
	return leafServer(t, false, func(w nethttp.ResponseWriter, r *nethttp.Request) {
		got, bad := 0, 0
		buf := make([]byte, 32768)
		for {
			n, err := r.Body.Read(buf)
			for i := 0; i < n; i++ {
				if buf[i] != byte((got+i)%251) {
					bad++
				}
			}
			got += n
			if err != nil {
				break
			}
		}
		if got != size || bad != 0 || r.ContentLength != int64(size) || r.Header.Get("Content-Type") != "application/octet-stream" {
			t.Errorf("server: got %d bad %d, Content-Length %d, Content-Type %q", got, bad, r.ContentLength,
				r.Header.Get("Content-Type"))
		}
		w.WriteHeader(201)
	})
}

// Upload streams a file from /flash with Content-Length and the Bearer
// token (taken from config upload.token), 4 KB at a time: the server
// checks every byte, and the token never shows in the VM's output. The
// VM's heap peak is the same for 1 MB as for 16 KB: bounded, not growing
// with the file. A missing file is fs.NotFound, before any connection.
func TestHTTPSUpload(t *testing.T) {
	const token = "s3cret-Token.42"
	peaks := map[int]int64{}
	for _, size := range []int{16 << 10, 1 << 20} {
		s := upServer(t, size)
		r := runHTTPS(t, map[uint16]string{443: s.addr}, map[string][]byte{"rec.bin": upPattern(size)},
			"time UP:https://gogo.test/up UPMISS:https://gogo.test/missing",
			"test.file=/flash/rec.bin\nupload.token="+token+"\n",
			"start true\ntime true\nUP https://gogo.test/up 201 0\nUPMISS https://gogo.test/missing 0 1\ndone\n")
		_, reqs := s.count()
		if want := `POST /up HTTP/1.1 gogo.test "close" "Bearer ` + token + `"`; len(reqs) != 1 || reqs[0] != want {
			t.Errorf("requests %q, want [%q]", reqs, want)
		}
		if strings.Contains(r.v.Stdout.String()+r.v.Stderr.String(), token) {
			t.Errorf("the token shows in the VM's output")
		}
		t.Logf("%d B upload: TLS heap peak %d B, VM heap peak %d B, longest read/write slice %d us", size,
			r.stats["heap_peak"], r.stats["vm_peak"], r.stats["max_io_us"])
		if r.stats["heap_peak"] > 48<<10 {
			t.Errorf("TLS heap peak %d B", r.stats["heap_peak"])
		}
		peaks[size] = r.stats["vm_peak"]
	}
	// The two may differ by a chunk or two (when a Done comes
	// relative to the next Read), not by the file.
	if d := peaks[1<<20] - peaks[16<<10]; d > 8192 {
		t.Errorf("VM heap peak %d B for 1 MB, %d B for 16 KB: grows with the file", peaks[1<<20], peaks[16<<10])
	}
}

// An upload whose connection fails says why: the port.Closed reason, a
// shared code, not a blanket IO. A host that never answers the SYN (no
// server behind port 9) is Timeout (9) when the connect gives up; a server
// whose certificate is for another name is BadCert (21).
func TestHTTPSUploadReasons(t *testing.T) {
	s := upServer(t, 1024)
	runHTTPS(t, map[uint16]string{443: s.addr}, map[string][]byte{"rec.bin": upPattern(1024)},
		"time UP:https://gogo.test:9/up UP:https://other.test/up",
		"test.file=/flash/rec.bin\n",
		"start true\ntime true\nUP https://gogo.test:9/up 0 9\nUP https://other.test/up 0 21\ndone\n")
	if conns, _ := s.count(); conns > 1 {
		t.Errorf("%d connections reached the server, want at most the refused handshake", conns)
	}
}

// A server that answers 401 before reading the body: Upload stops sending
// and returns the status. The server then closes its side and reads the
// rest until the VM's close, as HTTP servers do (a lingering close): a
// close with unread body bytes would reset the connection, and a reset
// discards the 401 if it reaches the bridge's socket before the bridge
// has read it (on a loaded host the VM then saw an IO error).
func TestHTTPSUploadDenied(t *testing.T) {
	srv := rawTLS(t, func(n int, c net.Conn, br *bufio.Reader) {
		if _, err := nethttp.ReadRequest(br); err != nil {
			return
		}
		io.WriteString(c, "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
		c.(*tls.Conn).CloseWrite()
		c.SetReadDeadline(time.Now().Add(realLimit))
		io.Copy(io.Discard, br)
	})
	runHTTPS(t, map[uint16]string{443: srv.ln.Addr().String()}, map[string][]byte{"rec.bin": upPattern(256 << 10)},
		"time UP:https://gogo.test/up", "test.file=/flash/rec.bin\n",
		"start true\ntime true\nUP https://gogo.test/up 401 0\ndone\n")
}

// Plain http goes through the same code: a one-shot Get is still HTTP/1.0
// with Connection: close; the Client keeps a plain connection too, and an
// upload over http works, but not with a token (BadURL: it would go in the
// clear). The image does not require tls (run without the tls driver).
func TestHTTPPlainSamePath(t *testing.T) {
	s := startHTTPServer(t, nil, false, func(w nethttp.ResponseWriter, r *nethttp.Request) {
		if r.Method == "POST" && r.URL.Path == "/up" {
			n, _ := io.Copy(io.Discard, r.Body)
			fmt.Fprintf(w, "%d", n)
			return
		}
		hello(w, r)
	})
	r := runHTTPS(t, map[uint16]string{80: s.addr}, map[string][]byte{"small.bin": upPattern(10000)},
		"GET:http://gogo.test/x new CGET:http://gogo.test/1 CGET:http://gogo.test/2 UP:http://gogo.test/up CUP:http://gogo.test/up cclose UPNT:http://gogo.test/up",
		"test.file=/flash/small.bin\nupload.token=secret\n",
		"start true\n"+
			"GET http://gogo.test/x true 200 8 false\n  body [hello /x]\n"+
			"new\n"+
			"CGET http://gogo.test/1 true 200 8 false\n  body [hello /1]\n"+
			"CGET http://gogo.test/2 true 200 8 false\n  body [hello /2]\n"+
			"UP http://gogo.test/up 0 25\nCUP http://gogo.test/up 0 25\n"+
			"cclose\nUPNT http://gogo.test/up 200 0\ndone\n",
		"--without", "tls") // an http image loads on a firmware without the tls driver
	conns, reqs := s.count()
	want := []string{`GET /x HTTP/1.0 gogo.test "close" ""`, `GET /1 HTTP/1.1 gogo.test "" ""`, `GET /2 HTTP/1.1 gogo.test "" ""`,
		`POST /up HTTP/1.1 gogo.test "close" ""`}
	if conns != 3 || fmt.Sprint(reqs) != fmt.Sprint(want) {
		t.Errorf("connections %d, requests %q; want 3, %q", conns, reqs, want)
	}
	if r.stats["opens"] != 0 {
		t.Errorf("plain http used the tls port")
	}
	_ = sim.DNSPort
}

// On a firmware without the "tls" driver (--without tls; the image does
// not require tls, gogo/http declares it //gogo:optional), https
// fails cleanly, one-shot and on a Client, and http still works, on the
// same Client too. Nothing reaches the https server.
func TestHTTPSNoTLSDriver(t *testing.T) {
	s := leafServer(t, false, hello)
	p := startHTTPServer(t, nil, false, hello)
	r := runHTTPS(t, map[uint16]string{443: s.addr, 80: p.addr}, nil,
		"time GET:https://gogo.test/a new CGET:https://gogo.test/b CGET:http://gogo.test/c cclose GET:http://gogo.test/d", "",
		"start true\ntime true\n"+
			"GET https://gogo.test/a false 0 0 false\n  body []\n"+
			"new\n"+
			"CGET https://gogo.test/b false 0 0 false\n  body []\n"+
			"CGET http://gogo.test/c true 200 8 false\n  body [hello /c]\n"+
			"cclose\n"+
			"GET http://gogo.test/d true 200 8 false\n  body [hello /d]\n"+
			"done\n",
		"--without", "tls")
	if conns, _ := s.count(); conns != 0 || r.stats["opens"] != 0 {
		t.Errorf("https server saw %d connections, tls opens %d; want 0, 0", conns, r.stats["opens"])
	}
	if conns, _ := p.count(); conns != 2 {
		t.Errorf("http server saw %d connections, want 2", conns)
	}
}

// While a Client holds the one TLS connection, a one-shot https request
// fails at open; after CloseClient it works. An upload on the Client's
// kept connection (ClientUpload) keeps it: one handshake for all three.
func TestHTTPSClientHoldsTLS(t *testing.T) {
	const size = 64 << 10
	s := leafServer(t, false, func(w nethttp.ResponseWriter, r *nethttp.Request) {
		if r.URL.Path == "/up" {
			b, _ := io.ReadAll(r.Body)
			if !bytesEqual(b, upPattern(size)) || r.Header.Get("Authorization") != "Bearer tok" {
				t.Errorf("upload: %d bytes, auth %q", len(b), r.Header.Get("Authorization"))
			}
			w.WriteHeader(201)
			return
		}
		hello(w, r)
	})
	r := runHTTPS(t, map[uint16]string{443: s.addr}, map[string][]byte{"rec.bin": upPattern(size)},
		"time new CGET:https://gogo.test/1 GET:https://gogo.test/2 CUP:https://gogo.test/up CGET:https://gogo.test/3 cclose GET:https://gogo.test/4",
		"test.file=/flash/rec.bin\nupload.token=tok\n",
		"start true\ntime true\nnew\n"+
			"CGET https://gogo.test/1 true 200 8 false\n  body [hello /1]\n"+
			"GET https://gogo.test/2 false 0 0 false\n  body []\n"+
			"CUP https://gogo.test/up 201 0\n"+
			"CGET https://gogo.test/3 true 200 8 false\n  body [hello /3]\n"+
			"cclose\n"+
			"GET https://gogo.test/4 true 200 8 false\n  body [hello /4]\n"+
			"done\n")
	if conns, reqs := s.count(); conns != 2 || len(reqs) != 4 || r.stats["handshakes"] != 2 {
		t.Errorf("connections %d, requests %q, handshakes %d", conns, reqs, r.stats["handshakes"])
	}
}

func bytesEqual(a, b []byte) bool { return string(a) == string(b) }

// Two actors share a Client: a request queued behind a slow one (a
// connect to a port nobody answers, 2 s) whose caller gives up meanwhile
// (300 ms) is never sent, even once the client is free; the client then
// serves the next request normally.
func TestHTTPSClientStaleRequest(t *testing.T) {
	s := leafServer(t, false, hello)
	runHTTPS(t, map[uint16]string{443: s.addr}, nil,
		"time new QSTALE:http://gogo.test:81/slow CGET:https://gogo.test/after cclose", "test.body=x\n",
		"start true\ntime true\nnew\n"+
			"CPOST https://gogo.test/stale false 0 0 false\n  body []\n"+
			"CGET http://gogo.test:81/slow false 0 0 false\n  body []\n"+
			"slow done\n"+
			"CGET https://gogo.test/after true 200 12 false\n  body [hello /after]\n"+
			"cclose\ndone\n")
	if _, reqs := s.count(); len(reqs) != 1 || !strings.HasPrefix(reqs[0], "GET /after ") {
		t.Errorf("server saw %q, want only /after", reqs)
	}
}

// A Client holds at most MaxQueued waiting requests: while it is busy
// (a connect nobody answers), MaxQueued more queue and one over that
// fails at once; none of them is ever sent (the queued ones are stale by
// the time the client is free), and the client then serves normally.
func TestHTTPSClientQueueFull(t *testing.T) {
	s := leafServer(t, false, hello)
	runHTTPS(t, map[uint16]string{443: s.addr}, nil,
		"time new QFULL:http://gogo.test:81/slow CGET:https://gogo.test/after cclose", "test.body=x\n",
		"start true\ntime true\nnew\n"+
			"over false true\n"+
			"CGET http://gogo.test:81/slow false 0 0 false\n  body []\n"+
			"qfull done\n"+
			"CGET https://gogo.test/after true 200 12 false\n  body [hello /after]\n"+
			"cclose\ndone\n")
	if _, reqs := s.count(); len(reqs) != 1 || !strings.HasPrefix(reqs[0], "GET /after ") {
		t.Errorf("server saw %q, want only /after", reqs)
	}
}

// A Client follows its creator: the creator exits without CloseClient,
// the client closes its connection and ends, and the TLS connection is
// free for the next https request.
func TestHTTPSClientFollowsCreator(t *testing.T) {
	s := leafServer(t, false, hello)
	runHTTPS(t, map[uint16]string{443: s.addr}, nil, "time CRASH:https://gogo.test/1 GET:https://gogo.test/2", "",
		"start true\ntime true\n"+
			"CGET https://gogo.test/1 true 200 8 false\n  body [hello /1]\n"+
			"creator down 100\nclient down 0\n"+
			"GET https://gogo.test/2 true 200 8 false\n  body [hello /2]\n"+
			"done\n")
}

// A Client's creator dies while its own request is in flight (the server
// holds the response back) and two other actors' requests are queued
// behind it: the in-flight one is aborted (its connection reset), the
// Client ends at once, and both queued requests fail within 100 ms of the
// death, unsent (the server sees only /hang). The one TLS slot is free
// afterwards: the next https Get goes through.
func TestHTTPSClientCreatorDiesQueued(t *testing.T) {
	var mu sync.Mutex
	ready := false
	s := leafServer(t, false, func(w nethttp.ResponseWriter, r *nethttp.Request) {
		if r.URL.Path == "/hang" {
			mu.Lock()
			ready = true
			mu.Unlock()
			<-r.Context().Done() // until the client resets the connection
			return
		}
		hello(w, r)
	})
	rs := readyServer(t, &mu, &ready)
	r := runHTTPS(t, map[uint16]string{443: s.addr, 80: rs.addr}, nil,
		"time QDEAD:https://gogo.test/hang WAIT:http://gogo.test/ready QUEUE2 QKILL GET:https://gogo.test/after", "",
		"start true\ntime true\nready\n"+
			"queued false true\nqueued false true\n"+
			"creator down 100 client down 0\n"+
			"GET https://gogo.test/after true 200 12 false\n  body [hello /after]\n"+
			"done\n")
	if _, reqs := s.count(); len(reqs) != 2 || !strings.HasPrefix(reqs[0], "GET /hang ") || !strings.HasPrefix(reqs[1], "GET /after ") {
		t.Errorf("server saw %q, want /hang then /after only", reqs)
	}
	if r.stats["handshakes"] != 2 || r.stats["opens"] != 2 {
		t.Errorf("handshakes %d, opens %d; want 2, 2", r.stats["handshakes"], r.stats["opens"])
	}
}

// Interim 1xx responses (103 Early Hints) are skipped: the real response
// follows, and the kept connection stays in step for the next request.
func TestHTTPSInterim(t *testing.T) {
	srv := rawTLS(t, func(n int, c net.Conn, br *bufio.Reader) {
		for {
			r := readReq(br)
			if r == "" {
				return
			}
			if r == "GET /hints" {
				io.WriteString(c, "HTTP/1.1 103 Early Hints\r\nLink: </x>\r\n\r\n")
			}
			io.WriteString(c, "HTTP/1.1 200 OK\r\nContent-Length: "+strconv.Itoa(len(r))+"\r\n\r\n"+r)
		}
	})
	runHTTPS(t, map[uint16]string{443: srv.ln.Addr().String()}, nil,
		"time new CGET:https://gogo.test/hints CGET:https://gogo.test/next cclose", "",
		"start true\ntime true\nnew\n"+
			"CGET https://gogo.test/hints true 200 10 false\n  body [GET /hints]\n"+
			"CGET https://gogo.test/next true 200 9 false\n  body [GET /next]\n"+
			"cclose\ndone\n")
}

// readyServer is a plain http server on which GET /ready answers "yes"
// once ready is set (the WAIT step of https_client).
// settle waits, up to realLimit, until done (called under mu) holds: a
// server handler sees its connection closed (by the bridge, when the VM
// resets it or exits) a moment after the VM has finished.
func settle(mu *sync.Mutex, done func() bool) {
	for deadline := time.Now().Add(realLimit); ; time.Sleep(time.Millisecond) {
		mu.Lock()
		ok := done()
		mu.Unlock()
		if ok || time.Now().After(deadline) {
			return
		}
	}
}

func readyServer(t *testing.T, mu *sync.Mutex, ready *bool) *httpSrv {
	return startHTTPServer(t, nil, false, func(w nethttp.ResponseWriter, r *nethttp.Request) {
		mu.Lock()
		defer mu.Unlock()
		if *ready {
			io.WriteString(w, "yes")
		} else {
			io.WriteString(w, "no")
		}
	})
}

// An Upload follows its caller: the caller is killed mid-upload (the link
// trickles after 24 KB, so the uploader waits for a write; the kill comes
// once the server has read 8 KB of the body); the uploader stops, resets
// its connection, and the one TLS connection is free at once for the
// restarted caller's Upload, which goes through.
func TestHTTPSUploadCallerCrash(t *testing.T) {
	const size = 256 << 10
	var mu sync.Mutex
	ready, slowRead, slowErr := false, -1, error(nil)
	s := leafServer(t, false, func(w nethttp.ResponseWriter, r *nethttp.Request) {
		if r.URL.Path == "/slow" {
			n, err := io.CopyN(io.Discard, r.Body, 8<<10)
			mu.Lock()
			ready = err == nil
			mu.Unlock()
			m, err := io.Copy(io.Discard, r.Body)
			mu.Lock()
			slowRead, slowErr = int(n+m), err
			mu.Unlock()
			return
		}
		if n, _ := io.Copy(io.Discard, r.Body); n != size {
			t.Errorf("upload: %d bytes", n)
		}
		w.WriteHeader(201)
	})
	rs := readyServer(t, &mu, &ready)
	r := runHTTPSStall(t, map[uint16]string{443: s.addr, 8443: s.addr, 80: rs.addr}, map[uint16]int{8443: 24 << 10},
		map[string][]byte{"rec.bin": upPattern(size)},
		"time UPCRASH:https://gogo.test:8443/slow WAIT:http://gogo.test/ready KILL UP:https://gogo.test/up",
		"test.file=/flash/rec.bin\nupload.token=tok\n",
		"start true\ntime true\nready\ncaller down 100\nUP https://gogo.test/up 201 0\ndone\n")
	settle(&mu, func() bool { return slowRead >= 0 })
	mu.Lock()
	defer mu.Unlock()
	if slowRead < 8<<10 || slowRead >= size || slowErr == nil {
		t.Errorf("the stalled upload: server read %d bytes (err %v), want a cut-short body", slowRead, slowErr)
	}
	if r.stats["handshakes"] != 2 {
		t.Errorf("handshakes %d, want 2", r.stats["handshakes"])
	}
}

// A one-shot Get follows its caller too: the caller is killed (long
// before its 60 s timeout) while the server holds the response back; the
// worker resets the connection, and the next https Get goes through at
// once.
func TestHTTPSGetCallerCrash(t *testing.T) {
	var mu sync.Mutex
	ready, ended := false, false
	s := leafServer(t, false, func(w nethttp.ResponseWriter, r *nethttp.Request) {
		if r.URL.Path == "/hang" {
			mu.Lock()
			ready = true
			mu.Unlock()
			<-r.Context().Done() // until the connection goes
			mu.Lock()
			ended = true
			mu.Unlock()
			return
		}
		hello(w, r)
	})
	rs := readyServer(t, &mu, &ready)
	runHTTPS(t, map[uint16]string{443: s.addr, 80: rs.addr}, nil,
		"time GETCRASH:https://gogo.test/hang WAIT:http://gogo.test/ready KILL GET:https://gogo.test/2",
		"test.timeout=60000\n",
		"start true\ntime true\nready\ncaller down 100\n"+
			"GET https://gogo.test/2 true 200 8 false\n  body [hello /2]\n"+
			"done\n")
	settle(&mu, func() bool { return ended })
	mu.Lock()
	defer mu.Unlock()
	if !ended {
		t.Errorf("the server never saw the held request end")
	}
}
