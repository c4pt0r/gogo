// Package http is a small HTTP/1.0 client and server (spec M5 §5.5,
// §5.6), written in the gogo subset and compiled into every image that
// imports it.
//
// Client: Get, Post and Do take an http:// URL (no https: they fail at
// once), resolve the host with dns.Lookup and send an HTTP/1.0 request
// with Host and "Connection: close". The response is read until the
// connection closes or its Content-Length is complete. The request runs
// in a temporary actor; the caller waits for it as actor.Call does, and on
// timeout the request is abandoned (its connection reset) and the result
// is false. A body longer than the limit (MaxBody unless Do says
// otherwise) is cut there and marked Truncated.
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
package http

import (
	"gogo/actor"
	"gogo/bytes"
	"gogo/dns"
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

// job is a client request, sent to the worker actor.
type job struct {
	Method, Host, HostPort, Path, ContentType string
	Port, MaxBody                             int
	Body                                      bytes.Bytes
	Deadline                                  int64
}

// result is the worker's reply.
type result struct {
	Resp Response
	OK   bool
}

// Get fetches url (http://host[:port][/path][?query][#fragment]) and
// returns the response; false if the URL is not http:// (or has user
// info, spaces or control bytes), the host does not resolve, the
// connection fails, or the response is malformed (a chunked one included:
// Transfer-Encoding is not decoded), contradictory or incomplete within
// timeoutMs (<= 0: DefaultTimeoutMs).
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
	host, hostPort, portNum, path, ok := parseURL(url)
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
	w := actor.SpawnOpt(worker, actor.Options{MaxMemory: memFor(maxBody)})
	r, ok := actor.Call(w, job{Method: method, Host: host, HostPort: hostPort, Path: path, ContentType: contentType,
		Port: portNum, MaxBody: maxBody, Body: body, Deadline: actor.Now() + int64(timeoutMs)}, timeoutMs)
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

// parseURL splits http://host[:port][/path][?query][#fragment] into the
// host, host[:port], port and request target (path and query).
func parseURL(url string) (string, string, int, string, bool) {
	if !strings.HasPrefix(url, "http://") {
		return "", "", 0, "", false
	}
	rest, i := url[7:], 0
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
	if found {
		n, ok = strconv.Atoi(p)
		ok = ok && n > 0 && n < 65536
	}
	return host, hostPort, n, path, ok && host != "" && strings.IndexByte(hostPort, '@') < 0 &&
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

// worker runs one client request: it answers the Call that brings the job.
func worker() {
	switch r := actor.Receive().(type) {
	case actor.Request:
		switch j := r.Body.(type) {
		case job:
			resp, ok := fetch(j)
			actor.Reply(r, result{resp, ok})
		}
	}
}

// fetch performs j; the caller gives up (and kills the worker) at
// j.Deadline, so fetch need not watch it except to stop waiting.
func fetch(j job) (Response, bool) {
	ip, ok := dns.Lookup(j.Host, int(j.Deadline-actor.Now()))
	left := int(j.Deadline - actor.Now())
	if !ok || left <= 0 {
		return Response{}, false
	}
	c, ok := port.Open("tcp", net.Dial{IP: ip, Port: j.Port, TimeoutMs: left})
	if !ok {
		return Response{}, false
	}
	b := bytes.NewBuffer(128)
	b.WriteString(j.Method + " " + j.Path + " HTTP/1.0\r\nHost: " + j.HostPort + "\r\nConnection: close\r\n")
	if j.ContentType != "" {
		b.WriteString("Content-Type: " + j.ContentType + "\r\n")
	}
	if j.Method != "GET" && j.Method != "HEAD" {
		b.WriteString("Content-Length: " + strconv.Itoa(j.Body.Len()) + "\r\n")
	}
	b.WriteString("\r\n")
	actor.Send(c, net.Write{Data: b.Freeze()})
	if j.Body.Len() > 0 {
		actor.Send(c, net.Write{Data: j.Body})
	}
	head := ""
	status, lim := -1, 0
	known, exact, truncated := false, false, false // Content-Length known; within the limit
	var hs []Header
	body := bytes.NewBuffer(0)
	for {
		left = int(j.Deadline - actor.Now())
		if left <= 0 {
			break
		}
		m, got := actor.ReceiveTimeout(left)
		if !got {
			break
		}
		switch e := m.(type) {
		case net.Closed:
			if e.Reason == net.Normal && status >= 0 && !known { // the body ran to the close
				actor.Send(c, net.Shutdown{})
				port.Close(c)
				return Response{status, hs, body.Freeze(), false}, true
			}
			port.Close(c)
			return Response{}, false
		case net.Data:
			d := e.Data
			if status < 0 {
				head += d.String()
				i := strings.Index(head, "\r\n\r\n")
				if i < 0 && len(head) <= 2*MaxHead {
					continue
				}
				n := -2
				if i >= 0 {
					status, hs, ok = parseStatus(head[:i])
					n = contentLength(hs)
				}
				_, chunked := HeaderValue(hs, "Transfer-Encoding")
				if i < 0 || i > 2*MaxHead || !ok || n == -2 || chunked {
					port.Close(c)
					return Response{}, false
				}
				lim, known = j.MaxBody, n >= 0
				if known && n <= lim {
					lim, exact = n, true
				}
				if j.Method == "HEAD" || status == 204 || status == 304 || (status >= 100 && status < 200) {
					lim, known, exact = 0, true, true
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
			if room := lim - body.Len(); d.Len() > room {
				d = d.Slice(0, room)
				truncated = !exact
			}
			body.WriteBytes(d)
			if exact && body.Len() == lim {
				actor.Send(c, net.Shutdown{})
				port.Close(c)
				return Response{status, hs, body.Freeze(), false}, true
			}
			if truncated {
				port.Close(c) // reset: the rest is not wanted
				return Response{status, hs, body.Freeze(), true}, true
			}
		}
	}
	port.Close(c)
	return Response{}, false
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
