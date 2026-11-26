// Package http is a small HTTP/1.x client and HTTP/1.0 server (spec M5
// §5.5, §5.6; HTTPS design 2026-11-25), written in the gogo subset and
// compiled into every image that imports it.
//
// Client: Get, Post and Do take an http:// or https:// URL, resolve the
// host with dns.Lookup and send a request with Host and "Connection:
// close": HTTP/1.0 over http, HTTP/1.1 over https, so the server frames
// the body (Content-Length or chunked; some, Cloudflare among them, answer
// an HTTP/1.0 request with a body delimited only by the close, which is
// refused over TLS). https uses the "tls" port (net.DialTLS: the
// certificate checked for the host, TLS 1.2; the wall clock must be set,
// see gogo/sntp) and http the "tcp" port, through the same code; the
// differences are the port (dial), its memory (connMemory), unframed
// bodies (refused over TLS, below), a Bearer token (https only) and the
// graceful close (finish waits for the peer over TLS). The
// response is read until its Content-Length is complete or, over http
// only, until the connection closes (over TLS a close can be forged at a
// record boundary, so an unframed or short body is an error there). The
// request runs in a temporary actor spawned with the memory it needs
// (connMemory: more for tls); the caller waits for it as actor.Call
// does, and on timeout the request is abandoned (its connection reset)
// and the result is false. The temporary actor monitors its caller: if
// the caller exits (or is killed) meanwhile, it stops, resets its
// connection and ends, so a dead caller never keeps a connection (or the
// one TLS connection) busy. A body longer than the limit (MaxBody unless
// Do says otherwise) is cut there and marked Truncated.
//
// NewClient starts a keep-alive client, which follows the actor that
// made it: ClientDo and ClientUpload send HTTP/1.1 requests over one kept
// connection (Content-Length or chunked responses; interim 1xx skipped),
// and it reconnects when the server closed it while idle. Upload streams a
// file as a POST body with Content-Length and an optional Bearer token
// (https only). At most one https connection exists at a time (the "tls"
// port's limit).
//
// Server: Serve starts a listener actor, linked to the caller, that hands
// each accepted connection to a fresh actor. That actor reads one request
// (head at most MaxHead bytes, body at most MaxBody bytes by its
// Content-Length; no chunked bodies), calls the handler, writes the
// response with Content-Length and "Connection: close", shuts the
// connection down and exits; the stack finishes the close. A malformed
// request gets 400 (413 for a body over MaxBody) without calling the
// handler. A handler that crashes takes only its connection (reset) with
// it. A connection that sends no complete request within ReadTimeoutMs is
// closed.
//
// Headers are matched case-insensitively (HeaderValue). The request and
// response limits make each request actor's memory bounded: it is spawned
// with a limit of 2*body limit + 6 KB (actor.SpawnOpt; 22 KB for
// MaxBody), as the body is assembled in a buffer and then copied once.
//
// The "tls" driver is optional (the directive below): importing gogo/http
// does not make an image require tls. On a firmware without it an https
// request fails (port.Open returns ok=false), as ClientDo/ClientUpload do,
// while http works.
//
//gogo:optional tls
package http

import (
	"gogo/actor"
	"gogo/bytes"
	"gogo/dns"
	"gogo/fs"
	"gogo/net"
	"gogo/port"
	"gogo/strconv"
	"gogo/strings"
)

// Header is one header line.
type Header struct {
	Name, Value string
}

// Request is what a Serve handler gets.
type Request struct {
	Method, Path string
	Headers      []Header
	Body         bytes.Bytes
}

// Response is what a Serve handler returns (Status 0 means 200; the
// server adds Content-Length and Connection itself and drops a handler's
// own) and what Get, Post and Do return.
type Response struct {
	Status    int
	Headers   []Header
	Body      bytes.Bytes
	Truncated bool
}

const (
	// MaxBody is the default body limit (client responses, server requests).
	MaxBody = 8192
	// MaxBodyLimit caps Do's maxBody.
	MaxBodyLimit = 65536
	// MaxHead is the server's limit on a request's line and headers; a
	// response head may be twice as long.
	MaxHead = 2048
	// ReadTimeoutMs bounds how long the server waits for a whole request.
	ReadTimeoutMs = 10000
	// DefaultTimeoutMs is the client's timeout for timeoutMs <= 0.
	DefaultTimeoutMs = 10000
	// ListenFailed is the exit code of a Serve listener that could not
	// listen (no network, port taken, or both listen slots in use); it
	// reaches the linked caller as an exit signal.
	ListenFailed = 100
)

// target is a parsed URL: http:// or https:// (Secure), the host, the
// Host header value (host[:port] as written), the port and the request
// target (path and query).
type target struct {
	Secure               bool
	Host, HostPort, Path string
	Port                 int
}

// job is a client request, sent to the actor that makes it (a worker, a
// Client or an uploader).
type job struct {
	Method, ContentType, Token string
	URL                        target
	MaxBody                    int
	Body                       bytes.Bytes
	Deadline                   int64
	V11                        bool   // HTTP/1.1 (chunked responses are decoded); else HTTP/1.0
	Keep                       bool   // keep the connection: no "Connection: close"
	File                       string // Upload: the file to send as the body
}

// result is a worker's or Client's reply.
type result struct {
	Resp Response
	OK   bool
}

// upResult is the uploader's reply.
type upResult struct {
	Status int
	Err    fs.Err
}

// closeReq asks a Client to end.
type closeReq struct{}

// Client is a keep-alive HTTP/1.1 client (NewClient): the PID of the
// actor that owns its connection.
type Client struct {
	PID actor.PID
}

// Errors of Upload beside the fs ones.
const (
	// Failed: no usable response (the host did not resolve, the
	// connection failed, closed or timed out, the response was malformed
	// or cut short).
	Failed fs.Err = 100
	// BadURL: the URL is not http:// or https://, or the content type
	// or token has bytes that cannot go in a header.
	BadURL fs.Err = 101
)

// TLSMemory is what a "tls" connection charges its owner (the TLS state
// and room for one Data): the actor that opens one is spawned with this
// much more MaxMemory than for "tcp".
const TLSMemory = 26624

// UploadChunk is the bytes Upload reads from the file and writes at a
// time; at most 2 such writes are in flight.
const UploadChunk = 4096

// uploadMaxBody is how much of an upload's response body is read.
const uploadMaxBody = 1024

// LingerMs bounds how long a graceful close over TLS waits for the peer's
// close before the port closes (see finish).
const LingerMs = 1000

// replySlackMs is how much longer than its own deadline a caller waits
// for the Client's reply.
const replySlackMs = 500

// Response reading outcomes.
const (
	failed     = 0 // no usable response: reset the connection
	done       = 1 // complete: close the connection gracefully
	reuse      = 2 // complete: the connection may carry the next request
	cutShort   = 3 // cut at the body limit (Truncated): reset the connection
	peerClosed = 4 // the peer closed the idle connection: close it gracefully
)

// Body framings.
const (
	byLength = 0
	byChunks = 1
	byClose  = 2
)

// Get fetches url (http:// or https://host[:port][/path][?query][#fragment])
// and returns the response; false if the URL is not http:// or https://
// (or has user info, spaces or control bytes), the host does not resolve,
// the connection fails (for https: also the certificate is not valid
// for the host, or the clock is not set), or the response is malformed (a
// chunked one included: HTTP/1.0 is not sent one), contradictory or
// incomplete within timeoutMs (<= 0: DefaultTimeoutMs).
//
// The response body is an off-heap Bytes charged in full to the caller
// (and to every actor it is sent to): the caller needs room for up to
// MaxBody bytes beside its own heap, or receiving the reply takes it past
// its memory limit and it exits with MailboxOverflow. Under the MCU's 16
// KB default limit, either call from an actor spawned with a larger
// limit (actor.SpawnOpt, MaxMemory) or use Do with a small maxBody.
func Get(url string, timeoutMs int) (Response, bool) {
	var none bytes.Bytes
	return Do("GET", url, "", none, MaxBody, timeoutMs)
}

// Post sends body with Content-Type contentType to url, as Get.
func Post(url, contentType string, body bytes.Bytes, timeoutMs int) (Response, bool) {
	return Do("POST", url, contentType, body, MaxBody, timeoutMs)
}

// Do sends a request with method and returns the response, its body cut
// at maxBody bytes (<= 0: MaxBody; at most MaxBodyLimit), as Get. A
// Content-Type header is sent if contentType is not empty, Content-Length
// for any method but GET and HEAD. A method or content type with spaces
// or control bytes (resp. control bytes) fails at once.
func Do(method, url, contentType string, body bytes.Bytes, maxBody, timeoutMs int) (Response, bool) {
	u, ok := parseURL(url)
	if !ok || method == "" || !safe(method, false) || !safe(contentType, true) {
		return Response{}, false
	}
	if timeoutMs <= 0 {
		timeoutMs = DefaultTimeoutMs
	}
	if maxBody <= 0 {
		maxBody = MaxBody
	} else if maxBody > MaxBodyLimit {
		maxBody = MaxBodyLimit
	}
	w := actor.SpawnOpt(worker, actor.Options{MaxMemory: connMemory(u.Secure, maxBody)})
	// https: HTTP/1.1, so the body is framed (see the package doc).
	r, ok := actor.Call(w, job{Method: method, ContentType: contentType, URL: u, MaxBody: maxBody, Body: body,
		Deadline: actor.Now() + int64(timeoutMs), V11: u.Secure}, timeoutMs+replySlackMs)
	switch x := r.(type) {
	case result:
		if ok {
			return x.Resp, x.OK
		}
	}
	actor.Kill(w, actor.Shutdown) // its connection, if any, is reset
	return Response{}, false
}

// memFor is the memory limit of an actor that assembles a body of up to
// maxBody bytes: the buffer, the frozen copy, and room for the rest (the
// segment being added, the head, the frames). Measured peaks for an 8 KB
// body: 19.7 KB (client, body cut at the limit), 17.9 KB (server).
func memFor(maxBody int) int {
	return 2*maxBody + 6144
}

// connMemory is the memory limit of an actor that opens a connection and
// reads a body of up to maxBody bytes on it: memFor, plus what the port
// charges its owner (TLSMemory for "tls"; "tcp" fits memFor's slack).
func connMemory(secure bool, maxBody int) int {
	if secure {
		return memFor(maxBody) + TLSMemory
	}
	return memFor(maxBody)
}

// parseURL splits http[s]://host[:port][/path][?query][#fragment].
func parseURL(url string) (target, bool) {
	secure, rest := false, ""
	if strings.HasPrefix(url, "http://") {
		rest = url[7:]
	} else if strings.HasPrefix(url, "https://") {
		secure, rest = true, url[8:]
	} else {
		return target{}, false
	}
	i := 0
	for i < len(rest) && rest[i] != '/' && rest[i] != '?' && rest[i] != '#' {
		i++
	}
	hostPort, path := rest[:i], rest[i:]
	path, _, _ = strings.Cut(path, "#")
	if path == "" || path[0] == '?' {
		path = "/" + path
	}
	host, p, found := strings.Cut(hostPort, ":")
	n, ok := 80, true
	if secure {
		n = 443
	}
	if found {
		n, ok = strconv.Atoi(p)
		ok = ok && n > 0 && n < 65536
	}
	return target{secure, host, hostPort, path, n}, ok && host != "" && strings.IndexByte(hostPort, '@') < 0 &&
		safe(hostPort, false) && safe(path, false)
}

// safe reports whether s has no control bytes (tab allowed only with
// spaces) and, unless spaces is true, no spaces: what may go into a
// request line or header without splitting it.
func safe(s string, spaces bool) bool {
	for i := 0; i < len(s); i++ {
		c := s[i]
		if c == 0x7f || (c < 0x20 && (c != '\t' || !spaces)) || (c == ' ' && !spaces) {
			return false
		}
	}
	return true
}

// contentLength is the Content-Length in hs: -1 if there is none, -2 if
// one is not a number >= 0 or two differ.
func contentLength(hs []Header) int {
	n := -1
	for _, h := range hs {
		if strings.EqualFold(h.Name, "Content-Length") {
			v, ok := strconv.Atoi(h.Value)
			if !ok || v < 0 || (n >= 0 && v != n) {
				return -2
			}
			n = v
		}
	}
	return n
}

// dial resolves u's host and opens a connection to it, "tls" for https
// and "tcp" for http: the one place the schemes differ. The caller owns
// the port; it needs connMemory(u.Secure, ...).
func dial(u target, deadline int64) (actor.PID, bool) {
	var none actor.PID
	ip, ok := dns.Lookup(u.Host, int(deadline-actor.Now()))
	left := int(deadline - actor.Now())
	if !ok || left <= 0 {
		return none, false
	}
	if u.Secure {
		c, ok := port.Open("tls", net.DialTLS{IP: ip, Port: u.Port, TimeoutMs: left, Host: u.Host})
		return c, ok
	}
	c, ok := port.Open("tcp", net.Dial{IP: ip, Port: u.Port, TimeoutMs: left})
	return c, ok
}

// requestHead is j's request line and headers for a body of length bytes
// (-1: no Content-Length).
func requestHead(j job, length int) bytes.Bytes {
	b := bytes.NewBuffer(160)
	version := " HTTP/1.0\r\nHost: "
	if j.V11 {
		version = " HTTP/1.1\r\nHost: "
	}
	b.WriteString(j.Method + " " + j.URL.Path + version + j.URL.HostPort + "\r\n")
	if !j.Keep {
		b.WriteString("Connection: close\r\n")
	}
	if j.Token != "" {
		b.WriteString("Authorization: Bearer " + j.Token + "\r\n")
	}
	if j.ContentType != "" {
		b.WriteString("Content-Type: " + j.ContentType + "\r\n")
	}
	if length >= 0 {
		b.WriteString("Content-Length: " + strconv.Itoa(length) + "\r\n")
	}
	b.WriteString("\r\n")
	return b.Freeze()
}

// sendRequest writes j (head and body) to c.
func sendRequest(c actor.PID, j job) {
	n := -1
	if j.Method != "GET" && j.Method != "HEAD" {
		n = j.Body.Len()
	}
	actor.Send(c, net.Write{Data: requestHead(j, n)})
	if j.Body.Len() > 0 {
		actor.Send(c, net.Write{Data: j.Body})
	}
}

// finish ends a connection after a response read with outcome how:
// gracefully if it is complete (done, reuse) or the peer closed it
// (peerClosed), else with a reset. Gracefully is Shutdown, then closing
// the port: the stack finishes a "tcp" close on its own, but a "tls" one
// is reset if the port closes before its close_notify went out, so after
// a complete response over TLS finish waits for the peer's close (the
// server closes after "Connection: close", or when it reads our
// close_notify), until the time until at most. Messages a Client keeps
// that arrive meanwhile are added to queue (see keepMsg).
func finish(c actor.PID, how int, secure bool, until int64, queue []any) []any {
	if how == done || how == reuse || how == peerClosed {
		actor.Send(c, net.Shutdown{})
	}
	waiting := secure && (how == done || how == reuse)
	for waiting {
		wait := int(until - actor.Now())
		if wait <= 0 {
			break
		}
		m, got := actor.ReceiveTimeout(wait)
		waiting = got
		switch m.(type) {
		case net.Closed:
			waiting = false
		case actor.DownMsg: // the caller (creator) ended: nobody to linger for
			waiting = false
			queue = keepMsg(queue, m)
		default:
			queue = keepMsg(queue, m)
		}
	}
	port.Close(c)
	return queue
}

// lingerUntil is when finish stops waiting for a peer's close: LingerMs
// from now, or deadline if that comes first.
func lingerUntil(deadline int64) int64 {
	t := actor.Now() + int64(LingerMs)
	if deadline < t {
		return deadline
	}
	return t
}

// worker runs one client request: it answers the Call that brings the job,
// following its caller (a DownMsg ends the exchange: see readResponse).
func worker() {
	switch r := actor.Receive().(type) {
	case actor.Request:
		actor.Monitor(r.From)
		switch j := r.Body.(type) {
		case job:
			resp, ok := fetch(j)
			actor.Reply(r, result{resp, ok})
		}
	}
}

// fetch performs j on a connection of its own; the caller gives up (and
// kills the worker) at j.Deadline, so fetch need not watch it except to
// stop waiting.
func fetch(j job) (Response, bool) {
	c, ok := dial(j.URL, j.Deadline)
	if !ok {
		return Response{}, false
	}
	sendRequest(c, j)
	var none bytes.Bytes
	resp, how, _ := readResponse(c, j, none, nil)
	finish(c, how, j.URL.Secure, lingerUntil(j.Deadline), nil)
	return resp, how != failed
}

// readResponse reads the response to j from c, starting with the bytes
// first already taken from it, until j.Deadline. Messages a Client keeps
// that arrive meanwhile are added to queue (keepMsg). Interim 1xx
// responses (but 101) are skipped. A body is framed by its
// Content-Length, by chunks (HTTP/1.1 only) or, over http only, by the
// close; a close before the frame is complete is a failure (over TLS a
// close at a record boundary looks normal, so the frame is the only
// defence against a cut-short response). A DownMsg (the actor that the
// reader follows, its caller or creator, ended) fails the exchange at once
// and is kept in queue.
func readResponse(c actor.PID, j job, first bytes.Bytes, queue []any) (Response, int, []any) {
	head := ""
	status, lim, framing := -1, 0, byLength
	exact, keep := false, false
	var hs []Header
	body := bytes.NewBuffer(0)
	phase, left, line := 0, 0, "" // chunks: 0 size line, 1 data, 2 CRLF after data, 3 trailers
	var empty bytes.Bytes
	d := first
	for {
		if d.Len() > 0 && status < 0 {
			head += d.String()
			i := strings.Index(head, "\r\n\r\n")
			if i < 0 && len(head) <= 2*MaxHead {
				d = empty
			} else {
				ok := false
				n := -2
				if i >= 0 && i <= 2*MaxHead {
					status, hs, ok = parseStatus(head[:i])
					n = contentLength(hs)
				}
				te, chunked := HeaderValue(hs, "Transfer-Encoding")
				if !ok || n == -2 || (chunked && (!j.V11 || n >= 0 || !strings.EqualFold(te, "chunked"))) {
					return Response{}, failed, queue
				}
				if status >= 100 && status < 200 && status != 101 { // interim (100 Continue, 103): the real one follows
					d = bytes.FromString(head[i+4:])
					head, status = "", -1
					continue
				}
				framing = byLength
				if j.Method == "HEAD" || status == 204 || status == 304 || status == 101 {
					n = 0
				} else if chunked {
					framing = byChunks
				} else if n < 0 {
					framing = byClose
					if j.URL.Secure {
						return Response{}, failed, queue
					}
				}
				conn, _ := HeaderValue(hs, "Connection")
				keep = j.Keep && framing != byClose && strings.HasPrefix(head, "HTTP/1.1") &&
					!strings.EqualFold(conn, "close")
				lim = j.MaxBody
				exact = framing == byLength && n <= lim
				if exact {
					lim = n
				}
				// Without a known length the buffer starts at 2 KB, or at
				// the largest even fraction of the limit up to that, so its
				// doublings end on the limit.
				start := lim
				for !exact && start > 2048 && start%2 == 0 {
					start /= 2
				}
				body = bytes.NewBuffer(start)
				d = bytes.FromString(head[i+4:])
				head = ""
			}
		}
		if status >= 0 && framing != byChunks {
			if room := lim - body.Len(); d.Len() > room {
				if !exact {
					body.WriteBytes(d.Slice(0, room))
					return Response{status, hs, body.Freeze(), true}, cutShort, queue
				}
				d = d.Slice(0, room)
				keep = false // bytes after the response: not HTTP we can follow
			}
			body.WriteBytes(d)
			if exact && body.Len() == lim {
				if keep {
					return Response{status, hs, body.Freeze(), false}, reuse, queue
				}
				return Response{status, hs, body.Freeze(), false}, done, queue
			}
		} else if status >= 0 {
			i := 0
			for i < d.Len() {
				if phase == 1 {
					k := d.Len() - i
					if k > left {
						k = left
					}
					if body.Len()+k > lim {
						body.WriteBytes(d.Slice(i, i+lim-body.Len()))
						return Response{status, hs, body.Freeze(), true}, cutShort, queue
					}
					body.WriteBytes(d.Slice(i, i+k))
					i += k
					left -= k
					if left == 0 {
						phase = 2
					}
					continue
				}
				e := i
				for e < d.Len() && d.At(e) != '\n' {
					e++
				}
				line += d.Slice(i, e).String()
				if len(line) > MaxHead {
					return Response{}, failed, queue
				}
				if e == d.Len() {
					break // the line goes on in the next Data
				}
				i = e + 1
				l := strings.TrimSpace(line)
				line = ""
				if phase == 0 {
					size, _, _ := strings.Cut(l, ";")
					v, ok := strconv.ParseInt64(strings.TrimSpace(size), 16)
					if !ok || v < 0 || v > MaxBodyLimit+1 || len(size) > 8 {
						return Response{}, failed, queue
					}
					if v == 0 {
						phase = 3
					} else {
						phase, left = 1, int(v)
					}
				} else if phase == 2 {
					if l != "" {
						return Response{}, failed, queue
					}
					phase = 0
				} else if l == "" { // the end of the trailers: complete
					if keep && i == d.Len() {
						return Response{status, hs, body.Freeze(), false}, reuse, queue
					}
					return Response{status, hs, body.Freeze(), false}, done, queue
				}
			}
		}
		wait := int(j.Deadline - actor.Now())
		if wait <= 0 {
			return Response{}, failed, queue
		}
		m, got := actor.ReceiveTimeout(wait)
		if !got {
			return Response{}, failed, queue
		}
		d = empty
		switch e := m.(type) {
		case net.Data:
			d = e.Data
		case net.Closed:
			if e.Reason == net.Normal && status >= 0 && framing == byClose { // the body ran to the close
				return Response{status, hs, body.Freeze(), false}, done, queue
			}
			return Response{}, failed, queue
		case actor.DownMsg:
			return Response{}, failed, keepMsg(queue, m)
		default:
			queue = keepMsg(queue, m)
		}
	}
}

// NewClient starts a keep-alive client: an actor that makes the requests
// given to ClientDo and ClientUpload over HTTP/1.1, one at a time, keeping
// the connection open after each response for the next request to the
// same scheme, host and port. It owns at most one connection: a request
// elsewhere closes the one it has first. A connection the server closed
// while idle is noticed and a new one opened, transparently; one that ends
// after a request went out on it is not retried (the server may have
// acted on it): the call fails, and the next one reconnects. A request
// whose caller has given up (its timeout passed while it waited behind
// others) is dropped unsent. Responses are cut at MaxBody, as Get's.
//
// The client follows the actor that called NewClient: when that actor
// exits (or crashes) without CloseClient, the client closes its connection
// and ends too, so a dead owner never keeps the one TLS connection. A
// request in progress then (even another actor's) fails at once, its
// connection reset, and the requests queued behind it fail at once,
// unsent: the client ends right after that abort.
//
// The client is spawned with the memory an https connection and an upload
// need (clientMemory: 60 KB), so it can be used from any actor. Requests
// that wait for it are held in its memory (their bodies too); beyond
// MaxQueued waiting at once, a new one fails at once. While it holds an
// https connection, no other "tls" connection can open (one at a time):
// Get, Post, Do and Upload to an https URL fail until CloseClient.
func NewClient() Client {
	creator := actor.Self()
	return Client{actor.SpawnOpt(func() { client(creator) }, actor.Options{MaxMemory: clientMemory})}
}

// clientMemory is a Client's memory limit: an https connection with a
// MaxBody response, and an upload's 3 chunks.
const clientMemory = 2*MaxBody + 6144 + TLSMemory + 3*UploadChunk

// MaxQueued is how many requests a Client holds while it is busy with
// another; one more fails at once.
const MaxQueued = 4

// ClientDo makes a request through c, as Do does with maxBody MaxBody,
// on c's kept connection; false also if c is closed (after timeoutMs).
func ClientDo(c Client, method, url, contentType string, body bytes.Bytes, timeoutMs int) (Response, bool) {
	u, ok := parseURL(url)
	if !ok || method == "" || !safe(method, false) || !safe(contentType, true) {
		return Response{}, false
	}
	if timeoutMs <= 0 {
		timeoutMs = DefaultTimeoutMs
	}
	r, ok := actor.Call(c.PID, job{Method: method, ContentType: contentType, URL: u, MaxBody: MaxBody, Body: body,
		Deadline: actor.Now() + int64(timeoutMs), V11: true, Keep: true}, timeoutMs+replySlackMs)
	switch x := r.(type) {
	case result:
		if ok {
			return x.Resp, x.OK
		}
	}
	return Response{}, false
}

// CloseClient closes c's connection gracefully and ends it. Requests still
// waiting for it fail.
func CloseClient(c Client) {
	if _, ok := actor.Call(c.PID, closeReq{}, DefaultTimeoutMs); !ok {
		actor.Kill(c.PID, actor.Shutdown)
	}
}

// client is a Client's actor, following creator: conn is its connection
// while open, to key (scheme://host:port); queue holds the messages it
// took while busy (keepMsg).
func client(creator actor.PID) {
	actor.Monitor(creator)
	var conn actor.PID
	open, key := false, ""
	var queue []any
	for {
		var m any
		if len(queue) > 0 {
			m, queue = queue[0], queue[1:]
		} else {
			m = actor.Receive()
		}
		switch r := m.(type) {
		case actor.Request:
			switch b := r.Body.(type) {
			case job:
				var resp Response
				e := fs.OK
				resp, e, conn, open, key, queue = clientFetch(b, conn, open, key, queue)
				if b.File != "" {
					actor.Reply(r, upResult{resp.Status, e})
				} else {
					actor.Reply(r, result{resp, e == fs.OK})
				}
				if creatorDown(queue) { // the creator ended meanwhile: the
					// requests queued behind it fail now, unsent
					clientEnd(conn, open, key, queue)
					return
				}
			case closeReq:
				clientEnd(conn, open, key, queue)
				actor.Reply(r, true)
				return
			}
		case actor.DownMsg: // the creator ended: so does the client
			clientEnd(conn, open, key, queue)
			return
		case net.Closed: // the server ended the idle connection
			if open {
				finish(conn, peerClosed, false, 0, nil)
				open = false
				queue = flush(queue)
			}
		case net.Data: // bytes on an idle connection: not HTTP we follow
			if open {
				finish(conn, failed, false, 0, nil)
				open = false
				queue = flush(queue)
			}
		}
	}
}

// creatorDown reports whether queue holds the creator's DownMsg (the only
// one a Client gets: it monitors nobody else).
func creatorDown(queue []any) bool {
	for _, m := range queue {
		switch m.(type) {
		case actor.DownMsg:
			return true
		}
	}
	return false
}

// clientEnd closes the Client's connection gracefully and fails the
// requests still waiting.
func clientEnd(conn actor.PID, open bool, key string, queue []any) {
	if open {
		queue = finish(conn, done, strings.HasPrefix(key, "https:"), actor.Now()+int64(LingerMs), queue)
	}
	for _, m := range queue {
		switch r := m.(type) {
		case actor.Request:
			refuse(r)
		}
	}
}

// keepMsg adds to a Client's queue what it must not lose while busy: a
// Request (refused at once beyond MaxQueued, but a CloseClient), or its
// creator's DownMsg. Anything else (a connection's events) is dropped.
func keepMsg(queue []any, m any) []any {
	switch r := m.(type) {
	case actor.Request:
		switch r.Body.(type) {
		case closeReq:
			return append(queue, m)
		}
		if len(queue) >= MaxQueued {
			refuse(r)
			return queue
		}
		return append(queue, m)
	case actor.DownMsg:
		return append(queue, m)
	}
	return queue
}

// refuse answers a Client request with a failure.
func refuse(r actor.Request) {
	switch b := r.Body.(type) {
	case job:
		if b.File != "" {
			actor.Reply(r, upResult{0, Failed})
			return
		}
	}
	actor.Reply(r, result{})
}

// flush takes what is left in the mailbox after a connection was closed
// (its last events) without waiting, keeping what keepMsg keeps.
func flush(queue []any) []any {
	for {
		m, got := actor.ReceiveTimeout(0)
		if !got {
			return queue
		}
		queue = keepMsg(queue, m)
	}
}

// clientFetch makes request j on the Client's connection (opening one if
// it has none for j's host) and returns the response with fs.OK (or why
// not) and the new state. Nothing is sent once j's caller has given up.
func clientFetch(j job, conn actor.PID, open bool, key string, queue []any) (Response, fs.Err, actor.PID, bool, string, []any) {
	if j.Deadline <= actor.Now() {
		return Response{}, Failed, conn, open, key, queue
	}
	var f fs.File
	size := 0
	if j.File != "" {
		var e fs.Err
		f, size, e = openFile(j.File)
		if e != fs.OK {
			return Response{}, e, conn, open, key, queue
		}
	}
	k := "http://"
	if j.URL.Secure {
		k = "https://"
	}
	k = k + j.URL.Host + ":" + strconv.Itoa(j.URL.Port)
	// An event of the idle connection that came after the last look (the
	// server closed it): drop it, nothing has been sent on it yet.
	for open {
		m, got := actor.ReceiveTimeout(0)
		if !got {
			break
		}
		switch m.(type) {
		case net.Closed:
			finish(conn, peerClosed, false, 0, nil)
			open = false
			queue = flush(queue)
		case net.Data:
			finish(conn, failed, false, 0, nil)
			open = false
			queue = flush(queue)
		default:
			queue = keepMsg(queue, m)
		}
	}
	if open && key != k {
		queue = finish(conn, done, strings.HasPrefix(key, "https:"), lingerUntil(j.Deadline), queue)
		open = false
		queue = flush(queue)
	}
	if !open && !creatorDown(queue) {
		c, ok := dial(j.URL, j.Deadline)
		if !ok {
			if j.File != "" {
				f.Close()
			}
			return Response{}, Failed, conn, false, "", queue
		}
		conn, open, key = c, true, k
	}
	// the close or the connect took the time left, or the creator ended
	// meanwhile (the Client ends after this; no dial then): send nothing
	if j.Deadline <= actor.Now() || creatorDown(queue) {
		if j.File != "" {
			f.Close()
		}
		return Response{}, Failed, conn, open, key, queue
	}
	resp, how, e, q := exchange(conn, j, f, size, queue)
	if j.File != "" {
		f.Close()
	}
	if how == reuse {
		return resp, fs.OK, conn, true, key, q
	}
	q = finish(conn, how, j.URL.Secure, lingerUntil(j.Deadline), q)
	return resp, e, conn, false, "", flush(q)
}

// exchange sends j on c, its body from j.Body or (j.File != "") size
// bytes of the open file f, and reads the response: the outcome (finish
// it), and fs.OK, Failed or the file's error.
func exchange(c actor.PID, j job, f fs.File, size int, queue []any) (Response, int, fs.Err, []any) {
	if j.File == "" {
		sendRequest(c, j)
		var none bytes.Bytes
		resp, how, q := readResponse(c, j, none, queue)
		if how == failed {
			return resp, how, Failed, q
		}
		return resp, how, fs.OK, q
	}
	early, sent, e, q := sendFile(c, j, f, size, queue)
	if e != fs.OK {
		return Response{}, failed, e, q
	}
	resp, how, q := readResponse(c, job{Method: j.Method, URL: j.URL, MaxBody: j.MaxBody, V11: j.V11, Keep: j.Keep,
		Deadline: waitUntil(j.Deadline)}, early, q)
	if sent < size && how != failed {
		how = cutShort // the server answered before the body was all sent: reset
	}
	if how == failed {
		return resp, how, Failed, q
	}
	return resp, how, fs.OK, q
}

// sendFile writes j's head and size bytes of f to c, UploadChunk at a
// time with at most 2 writes in flight. It stops early when the response
// begins (early: its first bytes); sent is what was written; fs.OK,
// Failed (the connection closed, a wait timed out, or a DownMsg came: the
// actor followed ended; it is kept in queue) or the file's error.
func sendFile(c actor.PID, j job, f fs.File, size int, queue []any) (bytes.Bytes, int, fs.Err, []any) {
	actor.Send(c, net.Write{Data: requestHead(j, size)})
	sent, inflight := 0, 1
	var early bytes.Bytes
	for (sent < size || inflight > 0) && early.Len() == 0 {
		for inflight < 2 && sent < size {
			d, e := f.Read(UploadChunk)
			if e == fs.OK && d.Len() == 0 {
				e = fs.IO // the file shrank
			}
			if e != fs.OK {
				return early, sent, e, queue
			}
			if d.Len() > size-sent {
				d = d.Slice(0, size-sent) // the file grew: send what Content-Length says
			}
			actor.Send(c, net.Write{Data: d})
			sent += d.Len()
			inflight++
		}
		wait := int(waitUntil(j.Deadline) - actor.Now())
		if wait <= 0 {
			return early, sent, Failed, queue
		}
		m, got := actor.ReceiveTimeout(wait)
		if !got {
			return early, sent, Failed, queue
		}
		switch x := m.(type) {
		case net.Written:
			inflight--
		case net.Data:
			early = x.Data
		case net.Closed:
			return early, sent, Failed, queue
		case actor.DownMsg:
			return early, sent, Failed, keepMsg(queue, m)
		default:
			queue = keepMsg(queue, m)
		}
	}
	return early, sent, fs.OK, queue
}

// waitUntil is the end of one wait of an upload: DefaultTimeoutMs from
// now, or deadline if that comes first.
func waitUntil(deadline int64) int64 {
	t := actor.Now() + int64(DefaultTimeoutMs)
	if deadline < t {
		return deadline
	}
	return t
}

// openFile opens path for reading and returns its size.
func openFile(path string) (fs.File, int, fs.Err) {
	f, e := fs.Open(path, fs.Read)
	if e != fs.OK {
		return f, 0, e
	}
	size, e := f.Size()
	if e != fs.OK {
		f.Close()
	}
	return f, size, e
}

// Upload sends the file at path to url as the body of a POST with
// Content-Type contentType and, if token is not "", "Authorization:
// Bearer <token>" (take it from the store, config.Get("upload.token"),
// rather than from the source; it is never logged). A token goes over
// https only: with an http:// URL it is BadURL, so a mistyped URL cannot
// send it in the clear. The file is streamed: read UploadChunk bytes at a
// time with fs.File.Read, at most 2 writes in flight, sent with its
// Content-Length, so it is never in memory whole. The request is HTTP/1.1
// with "Connection: close" on a connection of its own (ClientUpload sends
// it on a Client's kept connection).
//
// It returns the response status (anything the server answers, 2xx or
// not) with fs.OK; or 0 and the fs error of the file (opening, reading;
// a file that shrinks while it is sent is fs.IO), Failed (no usable
// response) or BadURL. A server that answers before it has read the whole
// body (401, 413) stops the upload and its status is returned. Each wait
// (connecting, a write, the response) is bounded by DefaultTimeoutMs, and
// the upload as a whole by 3*DefaultTimeoutMs plus 1 ms per byte: a link
// slower than about 1 KB/s fails (Failed).
//
// It runs in an actor of its own, spawned with what it needs
// (uploadMemory: 46 KB for https); the caller's own limit is not
// touched. That actor monitors the caller: if the caller exits (a crash,
// a supervisor's kill) before the upload ends, it stops sending, resets
// the connection (freeing the one TLS connection for a restarted caller's
// retry) and ends.
func Upload(url, path, contentType, token string) (int, fs.Err) {
	j, ms, e := uploadJob(url, path, contentType, token, false)
	if e != fs.OK {
		return 0, e
	}
	w := actor.SpawnOpt(uploader, actor.Options{MaxMemory: uploadMemory(j.URL.Secure)})
	r, ok := actor.Call(w, j, ms+replySlackMs)
	switch x := r.(type) {
	case upResult:
		if ok {
			return x.Status, x.Err
		}
	}
	actor.Kill(w, actor.Shutdown)
	return 0, Failed
}

// ClientUpload is Upload on c's kept connection (HTTP/1.1, the connection
// kept after the response, as ClientDo).
func ClientUpload(c Client, url, path, contentType, token string) (int, fs.Err) {
	j, ms, e := uploadJob(url, path, contentType, token, true)
	if e != fs.OK {
		return 0, e
	}
	r, ok := actor.Call(c.PID, j, ms+replySlackMs)
	switch x := r.(type) {
	case upResult:
		if ok {
			return x.Status, x.Err
		}
	}
	return 0, Failed
}

// uploadJob checks an upload's arguments and makes its job, with its
// deadline: 3 waits, plus 1 ms per byte (1 KB/s at the least), ms from now.
func uploadJob(url, path, contentType, token string, keep bool) (job, int, fs.Err) {
	u, ok := parseURL(url)
	if !ok || !safe(contentType, true) || !safe(token, false) || (token != "" && !u.Secure) {
		return job{}, 0, BadURL
	}
	info, e := fs.Stat(path)
	if e != fs.OK {
		return job{}, 0, e
	}
	if info.IsDir {
		return job{}, 0, fs.IsDir
	}
	ms := info.Size
	if ms > 2000000000 {
		ms = 2000000000
	}
	ms += 3 * DefaultTimeoutMs
	return job{Method: "POST", ContentType: contentType, Token: token, URL: u, V11: true, Keep: keep,
		MaxBody: uploadMaxBody, File: path, Deadline: actor.Now() + int64(ms)}, ms, fs.OK
}

// uploadMemory is the uploader's memory limit: the response (memFor of a
// small body), 3 chunks (2 in flight, 1 being read) and the port.
func uploadMemory(secure bool) int {
	return connMemory(secure, uploadMaxBody) + 3*UploadChunk
}

// uploader runs one Upload: it answers the Call that brings the job,
// following its caller (a DownMsg ends the exchange: see sendFile).
func uploader() {
	switch r := actor.Receive().(type) {
	case actor.Request:
		actor.Monitor(r.From)
		switch j := r.Body.(type) {
		case job:
			status, e := upload(j)
			actor.Reply(r, upResult{status, e})
		}
	}
}

// upload performs Upload's job j on a connection of its own.
func upload(j job) (int, fs.Err) {
	f, size, e := openFile(j.File)
	if e != fs.OK {
		return 0, e
	}
	c, ok := dial(j.URL, waitUntil(j.Deadline))
	if !ok {
		f.Close()
		return 0, Failed
	}
	resp, how, e, _ := exchange(c, j, f, size, nil)
	f.Close()
	finish(c, how, j.URL.Secure, lingerUntil(j.Deadline), nil)
	if e != fs.OK {
		return 0, e
	}
	return resp.Status, fs.OK
}

// parseStatus parses a response head: "HTTP/1.x NNN reason" and headers.
func parseStatus(head string) (int, []Header, bool) {
	line, rest, _ := strings.Cut(head, "\r\n")
	if len(line) < 12 || !strings.HasPrefix(line, "HTTP/1.") || line[8] != ' ' || (len(line) > 12 && line[12] != ' ') {
		return 0, nil, false
	}
	code, ok := strconv.Atoi(line[9:12])
	hs, good := parseHeaders(rest)
	return code, hs, ok && good && code >= 100
}

// parseHeaders parses "Name: value" lines separated by CRLF.
func parseHeaders(rest string) ([]Header, bool) {
	var hs []Header
	for rest != "" {
		var l string
		l, rest, _ = strings.Cut(rest, "\r\n")
		name, value, found := strings.Cut(l, ":")
		if !found || name == "" || !safe(name, false) || !safe(value, true) {
			return nil, false
		}
		hs = append(hs, Header{name, strings.TrimSpace(value)})
	}
	return hs, true
}

// HeaderValue returns the value of the first header called name, compared
// case-insensitively.
func HeaderValue(h []Header, name string) (string, bool) {
	for _, x := range h {
		if strings.EqualFold(x.Name, name) {
			return x.Value, true
		}
	}
	return "", false
}

// Serve listens on TCP port portNum and answers every request with
// handler, each in its own actor. It returns the listener, linked to the
// caller; if it cannot listen it exits with ListenFailed.
func Serve(portNum int, handler func(Request) Response) actor.PID {
	return actor.SpawnLink(func() { listen(portNum, handler) })
}

func listen(portNum int, handler func(Request) Response) {
	if _, ok := port.Open("tcp_listen", net.Listen{Port: portNum}); !ok {
		actor.Exit(ListenFailed)
	}
	for {
		switch m := actor.Receive().(type) {
		case net.Accepted:
			conn := m.Conn
			h := actor.SpawnOpt(func() { serveConn(conn, handler) }, actor.Options{MaxMemory: memFor(MaxBody)})
			if !port.Handoff(conn, h) {
				actor.Kill(h, actor.Shutdown) // not left waiting ReadTimeoutMs for nothing
				port.Close(conn)
			}
		}
	}
}

// serveConn answers one request on conn (handed to this actor).
func serveConn(conn actor.PID, handler func(Request) Response) {
	req, status := readRequest()
	if status == 0 {
		writeResponse(conn, handler(req), req.Method == "HEAD")
	} else if status > 0 {
		writeResponse(conn, Response{Status: status}, false)
	}
	actor.Send(conn, net.Shutdown{}) // exiting then leaves the close to the stack
}

// readRequest reads one request from the connection's events: status 0
// and the request, an error status to answer, or -1 to just close (the
// peer closed or the time ran out).
func readRequest() (Request, int) {
	deadline := actor.Now() + int64(ReadTimeoutMs)
	head := ""
	var req Request
	want := -1
	body := bytes.NewBuffer(0)
	for {
		left := int(deadline - actor.Now())
		if left <= 0 {
			return Request{}, -1
		}
		m, got := actor.ReceiveTimeout(left)
		if !got {
			return Request{}, -1
		}
		switch e := m.(type) {
		case net.Closed:
			return Request{}, -1
		case net.Data:
			d := e.Data
			if want < 0 {
				head += d.String()
				i := strings.Index(head, "\r\n\r\n")
				if i > MaxHead || (i < 0 && len(head) > MaxHead) {
					return Request{}, 400
				}
				if i < 0 {
					continue
				}
				r, ok := parseRequest(head[:i])
				n := contentLength(r.Headers)
				if _, has := HeaderValue(r.Headers, "Transfer-Encoding"); has || !ok || n == -2 {
					return Request{}, 400
				}
				if n < 0 {
					n = 0
				}
				if n > MaxBody {
					return Request{}, 413
				}
				req, want = r, n
				body = bytes.NewBuffer(n)
				d = bytes.FromString(head[i+4:])
				head = ""
			}
			if room := want - body.Len(); d.Len() > room {
				d = d.Slice(0, room) // anything after the body is ignored
			}
			body.WriteBytes(d)
			if body.Len() == want {
				return Request{req.Method, req.Path, req.Headers, body.Freeze()}, 0
			}
		}
	}
}

// parseRequest parses a request head: "METHOD /path HTTP/1.x" and headers.
func parseRequest(head string) (Request, bool) {
	line, rest, _ := strings.Cut(head, "\r\n")
	method, tail, found := strings.Cut(line, " ")
	path, version, found2 := strings.Cut(tail, " ")
	hs, ok := parseHeaders(rest)
	var none bytes.Bytes
	return Request{method, path, hs, none}, ok && found && found2 && method != "" &&
		strings.HasPrefix(path, "/") && safe(path, false) && strings.HasPrefix(version, "HTTP/1.")
}

// writeResponse writes r's status line, headers and body (not for HEAD)
// to conn. A status outside 100-999, or a header that is empty or has
// control bytes (or a space in its name), makes it a 500 instead.
func writeResponse(conn actor.PID, r Response, head bool) {
	status := r.Status
	if status == 0 {
		status = 200
	}
	for _, h := range r.Headers {
		if h.Name == "" || !safe(h.Name, false) || !safe(h.Value, true) {
			status = 0
		}
	}
	if status < 100 || status > 999 {
		writeResponse(conn, Response{Status: 500}, false)
		return
	}
	b := bytes.NewBuffer(128)
	b.WriteString("HTTP/1.0 " + strconv.Itoa(status) + " " + statusText(status) + "\r\nContent-Length: " +
		strconv.Itoa(r.Body.Len()) + "\r\nConnection: close\r\n")
	for _, h := range r.Headers {
		if !strings.EqualFold(h.Name, "Content-Length") && !strings.EqualFold(h.Name, "Connection") {
			b.WriteString(h.Name + ": " + h.Value + "\r\n")
		}
	}
	b.WriteString("\r\n")
	actor.Send(conn, net.Write{Data: b.Freeze()})
	if r.Body.Len() > 0 && !head {
		actor.Send(conn, net.Write{Data: r.Body})
	}
}

// statusText is the reason phrase of the common status codes ("" else).
func statusText(code int) string {
	if code == 200 {
		return "OK"
	} else if code == 201 {
		return "Created"
	} else if code == 204 {
		return "No Content"
	} else if code == 301 {
		return "Moved Permanently"
	} else if code == 302 {
		return "Found"
	} else if code == 304 {
		return "Not Modified"
	} else if code == 400 {
		return "Bad Request"
	} else if code == 403 {
		return "Forbidden"
	} else if code == 404 {
		return "Not Found"
	} else if code == 405 {
		return "Method Not Allowed"
	} else if code == 413 {
		return "Payload Too Large"
	} else if code == 500 {
		return "Internal Server Error"
	} else if code == 503 {
		return "Service Unavailable"
	}
	return ""
}
