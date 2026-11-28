# 10. Networking

The boards have Wi-Fi, and gogo has its own small TCP/IP stack written in
C (Ethernet, ARP, IPv4, ICMP echo, UDP and TCP), sized for a
microcontroller: its fixed state is under 4 KB. On top of it, libraries
written in the gogo subset give you DHCP, DNS, SNTP (the date) and an
HTTP client (HTTP/1.0, and HTTP/1.1 keep-alive) and server.

Networking follows the same model as everything else:

- The network interface, each UDP socket and each TCP connection is a
  **port**, owned by one actor, with typed events.
- Libraries such as DHCP and DNS are **actors** you talk to by message.
- A crashed connection handler takes only its own connection with it.

HTTPS (the `"tls"` port, keep-alive and file upload) has a chapter of its
own: [chapter 17](17-https.md).

## 10.1 Running network programs

Before a board can join your network, store the Wi-Fi name and password in
its configuration store (chapter 11; once per board, the settings survive
reflashing):

```sh
gogo config set wifi.ssid MyNetwork
gogo config set wifi.password -      # type the password; it is not echoed back
```

The firmware connects to Wi-Fi by itself, reconnects when the link drops,
and shows `gogo: wifi connected (...)` and later `gogo: ip <address>` on the
screen. Only 2.4 GHz networks work.

**On your computer, `gogo run` has no network.** The network ports refuse
to open, `net.Subscribe` returns false, and `dhcp.Start` fails at
once. The examples in this chapter check for that and say `no network`,
which is what you see when you run them on the host. gogo's own tests run
network programs against a simulated network (`tests/net`); the HTTP
server, UDP echo and TCP echo examples below were checked that way too,
and the results are shown with them.

> **Note.** With `GOGO_NET=host` in the environment (or `gogovm
> --host-net`), the host VM's `tcp` and `tcp_listen` ports use your
> computer's real sockets instead: a program can dial and accept TCP
> connections (numeric addresses; `netif`, `udp` and so DHCP and DNS stay
> unavailable). That is how a server node of distributed actors runs
> (chapter 20). The simulator in `tests/net` is driven from Go test code,
> not from `gogo run`.

## 10.2 Getting an address: gogo/dhcp

```go
func Start(cfg dhcp.Config) error  // Config{TimeoutMs int}; <= 0 means 10000
```

`dhcp.Start` starts the DHCP client actor, linked to the caller, and waits
until the interface has an address (it returns `nil`) or the timeout has
passed (`errors.Timeout`; the client keeps trying in the background). It
fails at once when there is no network (`port.Open`'s error for
`"netif"`). The client renews the lease, rebinds,
and starts over if the lease runs out. It owns the `"netif"` port, so keep
it alive (do not let the caller exit) while you use the network.

For a fixed address, set it in the configuration store and DHCP is
skipped:

```sh
gogo config set net.ip 192.168.1.50
gogo config set net.mask 255.255.255.0   # the default
gogo config set net.gw 192.168.1.1
gogo config set net.dns 192.168.1.1
```

### Watching the link

`net.Subscribe()` makes the caller receive `net.Up{IP, Gateway, DNS}`
whenever the interface gets (or changes) its address, and `net.Down{}`
when it loses it. It returns false if there is no network or 4 actors are
subscribed already.

IPv4 addresses are `int`s: `a.b.c.d` is `a<<24 | b<<16 | c<<8 | d`, so
addresses from 128.0.0.0 up are negative. Format them through `int64`:

```go
// net.Subscribe delivers net.Up when the interface gets an address and
// net.Down when it loses it. IP addresses are ints; format them yourself.
package main

import (
	"gogo/actor"
	"gogo/dhcp"
	"gogo/net"
	"gogo/strconv"
)

// ipString formats a.b.c.d held in an int (addresses from 128.0.0.0 up are
// negative ints, so work in int64).
func ipString(ip int) string {
	v := int64(ip)
	if v < 0 {
		v += 4294967296
	}
	s := ""
	for shift := 24; shift >= 0; shift -= 8 {
		if s != "" {
			s = s + "."
		}
		s = s + strconv.FormatInt64(v>>shift&255, 10)
	}
	return s
}

// ipv4 packs a.b.c.d into an int (wrapping past 2^31, as int does).
func ipv4(a, b, c, d int) int { return a<<24 | b<<16 | c<<8 | d }

func main() {
	println(ipString(ipv4(192, 168, 1, 42)), ipString(ipv4(10, 0, 0, 1)))
	if !net.Subscribe() {
		println("no network to watch")
		return
	}
	dhcp.Start(dhcp.Config{TimeoutMs: 1000}) // keeps trying in the background
	for {
		switch m := actor.Receive().(type) {
		case net.Up:
			println("up: ip", ipString(m.IP), "gateway", ipString(m.Gateway), "dns", ipString(m.DNS))
		case net.Down:
			println("down")
		}
	}
}
```

On the host:

```text
192.168.1.42 10.0.0.1
no network to watch
```

## 10.3 Names: gogo/dns

```go
func Lookup(name string, timeoutMs int) (int, error) // timeoutMs <= 0: 5000
```

`dns.Lookup` returns the IPv4 address of a name (an A record), asking the
DNS server learned from DHCP. A dotted address like `"10.0.0.1"` is returned
as is. Behind it is a resolver actor registered as `"dns"`, started by the
first lookup; it sends one query at a time, retries twice, and caches 4
answers by their time-to-live. You rarely call it directly: `http.Get` does.

To set the date from the network, see section 10.8.

## 10.4 HTTP client

```go
func Get(url string, timeoutMs int) (http.Response, error)
func Post(url, contentType string, body bytes.Bytes, timeoutMs int) (http.Response, error)
func Do(method, url, contentType string, body bytes.Bytes, maxBody, timeoutMs int) (http.Response, error)
func HeaderValue(h []http.Header, name string) (string, bool) // case-insensitive

type Response struct {
	Status    int
	Headers   []Header // Header{Name, Value string}
	Body      bytes.Bytes
	Truncated bool
}
```

The client speaks HTTP/1.0 with `Connection: close` to `http://` URLs, and
HTTP/1.1 with `Connection: close` to `https://` URLs (chapter 17 covers https, the keep-alive `http.Client` and
`http.Upload`). Each request runs in a temporary actor; the caller waits for it the way
`actor.Call` waits, and on timeout the request is abandoned. The temporary
actor follows its caller: if the caller exits or is killed meanwhile, the
request stops and its connection is reset. The error is an
`errors.Code`: `errors.BadURL` for a URL (or method or content type) that
cannot be sent, `errors.Timeout` when the timeout runs out,
`errors.IO` when DNS or the connection fails or the response is
malformed; an HTTP error status such as 404 is a response (`nil` error)
with that `Status`. `dns.Lookup` says more about a name: `errors.NotFound`
(no such name, no A record), `errors.NoRoute` (no network or DNS
server), `errors.Timeout`.

```go
// Fetch a page with http.Get. The URL comes from the config store
// (gogo config set fetch.url http://...); for https:// see chapter 17.
package main

import (
	"gogo/dhcp"
	"gogo/http"
	"gogo/sys"
)

func main() {
	url, ok := sys.Config("fetch.url")
	if !ok {
		url = "http://example.com/"
	}
	if dhcp.Start(dhcp.Config{}) != nil {
		println("no network")
		return
	}
	r, err := http.Get(url, 8000)
	if err != nil {
		println("GET", url, "failed:", err.Error())
		return
	}
	ct, _ := http.HeaderValue(r.Headers, "content-type")
	println("GET", url, "->", r.Status, ct, r.Body.Len(), "bytes", "truncated:", r.Truncated)
}
```

On the host:

```text
no network
```

**Response size.** The body is cut at `http.MaxBody` (8 KB) and marked
`Truncated`. `Do` takes a different limit, up to 64 KB. The whole body is
charged to the caller's memory, and the default limit on a board is 16 KB,
so a caller that wants big responses needs a larger `SpawnOpt` limit (or a
small `maxBody`). Over http, responses using chunked transfer encoding
fail (an HTTP/1.0 request must not get one); https requests and the
keep-alive client of chapter 17 decode them.

## 10.5 HTTP server

```go
func Serve(port int, handler func(http.Request) http.Response) actor.PID

type Request struct {
	Method, Path string
	Headers      []Header
	Body         bytes.Bytes
}
```

`http.Serve` starts a listener actor, linked to the caller. For every
connection it starts a **new actor** that reads one request, calls your
handler, sends the response with `Content-Length` and `Connection: close`,
and exits. So the handler runs in its own actor: a handler that crashes
loses only that one connection, and the server carries on. `Status` 0
means 200.

Limits: the request line and headers at most 2 KB, the body at most 8 KB
(by `Content-Length`; chunked bodies are not supported). Bad requests get
400 and too-large bodies 413 without calling the handler. A connection
that does not send a complete request within 10 s is closed. If the
listener cannot listen (the port is taken), it exits with code 69,
`http.ListenFailed` (`actor.Fail(errors.Busy)`: library failures are
exit codes 64..99, appendix B); trap exits to see that.

Since the handler is an ordinary function, you can call it directly to
test it, which is what this example does on the host:

```go
// A small web server: GET / shows a hit counter kept by its own actor;
// anything else is 404. On a board it serves on port 80 once DHCP (or the
// static net.ip setting) gives it an address. On the host there is no
// network, so it tests its handler by calling it directly instead.
package main

import (
	"gogo/actor"
	"gogo/bytes"
	"gogo/dhcp"
	"gogo/http"
	"gogo/strconv"
)

type hit struct{}

// hits counts requests; it is registered as "hits".
func hits() {
	n := 0
	for {
		switch r := actor.Receive().(type) {
		case actor.Request:
			n++
			actor.Reply(r, n)
		}
	}
}

// count asks the hits actor for the next number (0 if it is not there).
func count() int {
	p, ok := actor.Whereis("hits")
	if !ok {
		return 0
	}
	v, err := actor.Call(p, hit{}, 500)
	if err != nil {
		return 0
	}
	n, _ := v.(int)
	return n
}

// handle runs in a fresh actor for every connection.
func handle(r http.Request) http.Response {
	text := []http.Header{{"Content-Type", "text/plain"}}
	if r.Method != "GET" || r.Path != "/" {
		return http.Response{Status: 404, Headers: text, Body: bytes.FromString("not found\n")}
	}
	n := count()
	return http.Response{Headers: text, Body: bytes.FromString("hello from gogo, visitor " + strconv.Itoa(n) + "\n")}
}

func show(r http.Response) {
	status := r.Status
	if status == 0 {
		status = 200
	}
	body := r.Body.String()
	println(status, body[:len(body)-1]) // without the newline
}

func main() {
	actor.Register("hits", actor.Spawn(hits))
	if dhcp.Start(dhcp.Config{TimeoutMs: 10000}) != nil {
		println("no network: testing the handler directly")
		show(handle(http.Request{Method: "GET", Path: "/"}))
		show(handle(http.Request{Method: "GET", Path: "/"}))
		show(handle(http.Request{Method: "GET", Path: "/secret"}))
		return
	}
	actor.TrapExit(true) // Serve's listener is linked to us
	http.Serve(80, handle)
	println("serving on port 80")
	for {
		switch m := actor.Receive().(type) {
		case actor.ExitMsg:
			if m.Reason.Code == http.ListenFailed {
				println("port 80 is taken")
			}
		}
	}
}
```

On the host:

```text
no network: testing the handler directly
200 hello from gogo, visitor 1
200 hello from gogo, visitor 2
404 not found
```

On the simulated network (with `net.ip` set), three requests to the server
got these responses:

```text
GET / HTTP/1.0   ->  HTTP/1.0 200 OK
                     Content-Length: 27
                     Connection: close
                     Content-Type: text/plain

                     hello from gogo, visitor 1
GET / HTTP/1.0   ->  ... hello from gogo, visitor 2
GET /x HTTP/1.0  ->  HTTP/1.0 404 Not Found ... not found
```

and the program printed `serving on port 80`.

Note how the counter works: handlers run in separate, short-lived actors
and cannot share a variable, so the count lives in a `hits` actor that
handlers ask with `Call`. That is the actor way to share state.

`examples/netdemo` in the gogo tree is a fuller program for the M5StickS3:
a bilingual page showing button presses and uptime, plus a periodic
`http.Get`.

## 10.6 UDP

```go
port.Open("udp", net.UDP{Port: 7})             // Port 0: pick a free one
actor.Send(u, net.SendTo{Addr: a, Data: data}) // command
// event to the owner:
type Datagram struct{ From net.Addr; Data bytes.Bytes } // Addr{IP, Port int}
```

```go
// A UDP echo server on port 7, straight on the "udp" port. The DHCP actor
// owns the network interface; this actor owns the UDP port.
package main

import (
	"gogo/actor"
	"gogo/dhcp"
	"gogo/net"
	"gogo/port"
)

func main() {
	if dhcp.Start(dhcp.Config{}) != nil {
		println("no network")
		return
	}
	u, err := port.Open("udp", net.UDP{Port: 7})
	if err != nil {
		println("udp port 7 is taken")
		return
	}
	println("echoing on udp port 7")
	for i := 0; i < 3; i++ {
		switch d := actor.Receive().(type) {
		case net.Datagram:
			println("echo", d.Data.Len(), "bytes to port", d.From.Port)
			actor.Send(u, net.SendTo{Addr: d.From, Data: d.Data})
		}
	}
	println("done")
}
```

On the host:

```text
no network
```

On the simulated network, three datagrams of `hello` came back to their
sender, and the program printed:

```text
echoing on udp port 7
echo 5 bytes to port 5000
echo 5 bytes to port 5000
echo 5 bytes to port 5000
done
```

UDP has no flow control: a datagram that would take its owner over its
memory limit is dropped (the owner is not killed). A datagram to a host
whose hardware address is not known yet is dropped while the stack asks
for it with ARP; send again.

## 10.7 TCP

TCP connections are ports too, and they speak the stream protocol every
byte port shares (section 7.6): bytes in are `port.Chunk`, bytes out are
`port.Out`, each answered by `port.Done`, and the end is `port.Closed`.

| Port | Config | Commands | Events to the owner |
|---|---|---|---|
| `"tcp"` | `net.Dial{IP, Port, TimeoutMs}` | `port.Out{Data}`, `net.Shutdown{}` | `net.Connected{}`, `port.Chunk{From, Seq, ..., Data}`, `port.Done{From, Seq}`, `port.Closed{From, Reason}` |
| `"tcp_listen"` | `net.Listen{Port}` | | `net.Accepted{Conn}` |

- **Dialing.** `port.Open("tcp", net.Dial{...})` starts connecting; you get
  `Connected`, or `Closed` with `net.Refused`, `net.Timeout` or
  `net.NoRoute`.
- **Accepting.** A listener's owner gets `Accepted{Conn}` for each new
  connection. `Conn` is a `"tcp"` port owned by the listener's owner and
  **paused** until you `port.Handoff` it to the actor that will handle it
  (or send it `net.Resume{}` to keep it).
- **Receiving.** Each `Chunk` is the next segment from the peer, in order;
  `Dropped` is always 0. A segment is acknowledged only once its `Chunk`
  is in your mailbox, so a slow reader slows the sender down instead of
  being killed or losing data.
- **Sending.** `Out{Data}` sends a `Bytes`; `Done{Seq}` follows when the
  peer has acknowledged all of it. One `Out` may wait behind the one in
  progress; **a third `Out` before a `Done` is refused** with
  `Done{Seq: -1}` and not sent; the connection carries on. An empty
  `Out`, one after `Shutdown`, or one on a connection that has ended gets
  `Done{-1}` too.
- **Closing.** `Closed{Normal}` means the peer has finished sending (you may
  still send). Any other reason means the connection is gone; `Out`s still
  in flight then get `Done{-1}` before the `Closed`. Send `Shutdown{}` to
  finish your side, then close the port: the stack completes the close on
  its own. Closing the port without `Shutdown` (or exiting) resets the
  connection.
- **From.** Every event carries the connection's PID in `From`, so an
  actor that owns several connections can tell them apart.

The write rule matters in practice. This echo server queues data in a
`Buffer` while two Outs are in flight:

```go
// A TCP echo server on port 7: the listener accepts connections and hands
// each one to its own actor, which echoes until the peer closes.
package main

import (
	"gogo/actor"
	"gogo/bytes"
	"gogo/dhcp"
	"gogo/net"
	"gogo/port"
)

// echo owns one connection. A tcp port takes at most two Outs in flight
// (a third is refused with Done{Seq: -1} and not sent), so data that
// arrives while two are in flight waits in a Buffer.
func echo(conn actor.PID) {
	inFlight := 0
	waiting := bytes.NewBuffer(256)
	for {
		switch m := actor.Receive().(type) {
		case port.Chunk:
			if inFlight < 2 {
				actor.Send(conn, port.Out{Data: m.Data})
				inFlight++
			} else {
				waiting.WriteBytes(m.Data)
			}
		case port.Done:
			inFlight--
			if waiting.Len() > 0 {
				actor.Send(conn, port.Out{Data: waiting.Freeze()})
				inFlight++
			}
		case port.Closed:
			if m.Reason == net.Normal {
				// The peer has finished sending: finish our side too.
				actor.Send(conn, net.Shutdown{})
			}
			port.Close(conn) // after Shutdown the stack completes the close
			return
		}
	}
}

func main() {
	if dhcp.Start(dhcp.Config{}) != nil {
		println("no network")
		return
	}
	_, err := port.Open("tcp_listen", net.Listen{Port: 7})
	if err != nil {
		println("port 7 is taken")
		return
	}
	for {
		switch m := actor.Receive().(type) {
		case net.Accepted:
			conn := m.Conn
			h := actor.Spawn(func() { echo(conn) })
			if !port.Handoff(conn, h) {
				port.Close(conn)
			}
		}
	}
}
```

On the host:

```text
no network
```

On the simulated network a client sent five segments without
acknowledging the echoes, and got `one two three four five` back in full,
followed by the server's FIN. A naive echo (one `Out` per `Chunk`) would
lose data in the same exchange: the third `Out` arrives before the first
`Done` and is refused.

The stack is deliberately small: one segment in flight in each direction
(1460 bytes), in-order segments only, retransmission after 1 s doubling to
8 s, and giving up after 6 retransmissions (`Closed{Timeout}`); a peer
that closes its window but answers the stack's probes keeps the connection
however long it stays closed. There are
4 connection slots in all, and incoming connections never take the last
one, so the device can always dial out.

A connection with no traffic stays open as long as the peer is alive,
however long that is: after 2 minutes in which nothing arrived from the
peer the stack sends it a keepalive probe, an empty segment any live TCP
answers, and repeats it every 10 s. If 3 probes in a row go unanswered
(the peer was switched off, unplugged, or dropped by a NAT without a
reset) the connection is reset and its owner gets `Closed{Timeout}`, so
a dead peer frees its slot about 2.5 minutes after it went silent; you
do not need your own timer for that, only for an application-level
timeout. Data the stack cannot deliver because your mailbox is full does
not count as traffic: an owner that stops reading for 2.5 minutes loses
the connection the same way (one that reads again in time keeps it). With `--host-net` the operating system's
TCP keepalive does the same at the same timings. All connections end with
`Closed{NoRoute}` when the interface loses its address, or when the
`"netif"` owner (the DHCP actor) exits.

## 10.8 The date: gogo/sys and gogo/sntp

```go
// gogo/sys
func Time() int64        // Unix time in ms (UTC), or 0 if unknown
func SetTime(ms int64) bool
func Random() int        // 30 random bits: 0 <= n < 1<<30

// gogo/sntp
func Sync(server string, timeoutMs int) error // timeoutMs <= 0: 5000
func Now() int64                             // sys.Time()
```

The boards have no battery-backed clock, so after boot nothing knows the
date. `actor.Now` only counts milliseconds since the VM started. The
**wall clock** in `gogo/sys` is separate: `sys.Time()` returns the Unix
time in milliseconds, or 0 while it is unknown. Once set, it keeps
advancing by itself.

`sys.SetTime(ms)` sets it and returns true, with one exception: a time
earlier than the moment the firmware (or the host VM) was **built** is
refused, `SetTime` returns false and the clock is unchanged. That floor is
the only protection the clock has. Any code may set it, so nothing can move
it back to before the build. `SetTime` with 0 or less forgets the time, and
`Time` returns 0 again.

`sntp.Sync(server, timeoutMs)` asks an SNTP server (a name, resolved with
`gogo/dns`, or a dotted address; UDP port 123) for the time and sets the
wall clock. It runs the exchange in a helper actor, so your mailbox is not
touched, sends the request up to 3 times within the timeout, and corrects
for half the round trip. It returns an error, leaving the clock alone, if
the name does not resolve (`dns.Lookup`'s error), no valid reply comes in
time (`errors.Timeout`), or the floor refuses the answer
(`errors.BadArg`). SNTP is not authenticated, so a reply could come from anyone on
the network path. Sync once at startup, and again every few hours if the
date matters; the clock drifts with the board's crystal in between.

`sys.Random()` returns 30 bits from the platform's entropy source (the
chip's hardware generator on the board, `getentropy` on the host). It is
meant for protocol nonces and IDs; `gogo/sntp` uses it for its request.

```go
// The wall clock and randomness: gogo/sys, set from the network with
// gogo/sntp. The server comes from the config store
// (gogo config set clock.server pool.ntp.org).
package main

import (
	"gogo/dhcp"
	"gogo/sntp"
	"gogo/strconv"
	"gogo/sys"
)

// pad2 formats 0..99 with two digits.
func pad2(n int) string {
	if n < 10 {
		return "0" + strconv.Itoa(n)
	}
	return strconv.Itoa(n)
}

// date formats Unix time ms (UTC) as "YYYY-MM-DD hh:mm"
// (the days-to-civil algorithm, for dates after 1970).
func date(ms int64) string {
	mins := ms / 60000
	days := int(mins / 1440)
	hm := int(mins % 1440)
	z := days + 719468
	era := z / 146097
	doe := z - era*146097
	yoe := (doe - doe/1460 + doe/36524 - doe/146096) / 365
	doy := doe - (365*yoe + yoe/4 - yoe/100)
	mp := (5*doy + 2) / 153
	d := doy - (153*mp+2)/5 + 1
	m := mp + 3
	if m > 12 {
		m -= 12
	}
	y := yoe + era*400
	if m <= 2 {
		y++
	}
	return strconv.Itoa(y) + "-" + pad2(m) + "-" + pad2(d) + " " + pad2(hm/60) + ":" + pad2(hm%60)
}

func main() {
	server, ok := sys.Config("clock.server")
	if !ok {
		server = "pool.ntp.org"
	}
	if dhcp.Start(dhcp.Config{}) == nil && sntp.Sync(server, 5000) == nil {
		println("synced with", server, "->", date(sys.Time()), "UTC")
	} else {
		println("not synced: no network or no answer")
	}

	// Forget the time, then set it by hand.
	sys.SetTime(0)
	println("known:", sys.Time() != 0)
	noon := int64(4102488000000) // 2100-01-01 12:00 UTC
	println("set 2100:", sys.SetTime(noon))
	println("now:", date(sys.Time()))
	// Earlier than the build: refused, the clock is unchanged.
	println("set 1970:", sys.SetTime(1000))
	println("still:", date(sys.Time()))

	// 30 random bits.
	a, b := sys.Random(), sys.Random()
	println("random in range:", a >= 0 && a < 1<<30, "differ:", a != b)
}
```

On the host there is no network, so `Sync` fails, and the rest runs as on
a board:

```text
not synced: no network or no answer
known: false
set 2100: true
now: 2100-01-01 12:00
set 1970: false
still: 2100-01-01 12:00
random in range: true differ: true
```

On a board with Wi-Fi the first line is `synced with pool.ntp.org ->
<the date> UTC` instead. On the host the wall clock starts from the
computer's clock (or from `$GOGO_UNIX_TIME_MS`, in ms; 0 means unknown), so
`sys.Time()` is known from the start. The example forgets it first to show
the board's behaviour. On a board it starts unknown at every boot and reset.
gogo has no time zones and no date formatting. `date` above is the usual
days-to-civil arithmetic, valid for dates after 1970.

## 10.9 Memory on the board

On the M5StickS3 the Wi-Fi driver takes about 30 KB of heap and 31 KB of
static memory, gogo's stack under 4 KB, and each HTTP connection a handler
actor of about 3 KB plus its body buffers (handler and client actors are
started with a limit of 2 x body limit + 6 KB, 22 KB by default). Running
`examples/netdemo` leaves about 173 KB of internal RAM free.
