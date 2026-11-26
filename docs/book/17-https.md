# 17. HTTPS

Chapter 10's HTTP client also speaks HTTPS. Give `http.Get`, `http.Post`
or `http.Do` an `https://` URL and the request goes over TLS. On top of
that, `gogo/http` has two things a device that talks to a server all day
needs:

- a **keep-alive client**, `http.Client`, which keeps one connection open
  between requests, so a poll once a second does not pay for a TLS
  handshake every time;
- **`http.Upload`**, which streams a file (a recording, a log) from
  storage as a request body, a few KB at a time, so a file of any size fits
  in a small actor.

What there is not: a TLS server, client certificates, HTTP/2, TLS 1.3 (the
client speaks TLS 1.2, which every mainstream server accepts), and more
than **one TLS connection at a time** on a board.

## 17.1 Before the first request: the clock

TLS checks the server's certificate: that a trusted authority signed it,
that it is for the name in the URL, and that it is valid **now**. The board
has no battery-backed clock, so after boot it does not know the date, and
until it does every https request fails. Set the clock once the network is
up, with `gogo/sntp` (chapter 10):

```go
if !sntp.Sync("pool.ntp.org", 5000) {
	println("no time: https would fail")
	return
}
```

You do not need to sync again for TLS: the clock keeps running from there.

The authorities a board trusts are the usual public ones (the ESP-IDF
certificate bundle, in flash). A server with a certificate from Let's
Encrypt or another public CA works as it is; this includes a server
published with Tailscale Funnel. A self-signed certificate does not, and
there is no switch to turn the checks off.

## 17.2 https with Get, Post and Do

Nothing changes in the calls; the URL decides:

```go
// Fetch a page over HTTPS. The clock comes first: TLS checks the
// certificate's dates, and the board does not know the date after boot.
// The URL comes from the config store (gogo config set fetch.url
// https://...).
package main

import (
	"gogo/dhcp"
	"gogo/http"
	"gogo/sntp"
	"gogo/sys"
)

func main() {
	url, ok := sys.Config("fetch.url")
	if !ok {
		url = "https://example.com/"
	}
	if !dhcp.Start(dhcp.Config{}) {
		println("no network")
		return
	}
	if !sntp.Sync("pool.ntp.org", 5000) {
		println("no time: https would fail")
		return
	}
	// The request runs in an actor of its own with room for the TLS
	// state (about 23 KB); this actor only needs room for the body.
	r, ok := http.Get(url, 15000)
	if !ok {
		println("GET", url, "failed (DNS, connect, certificate or timeout)")
		return
	}
	println("GET", url, "->", r.Status, r.Body.Len(), "bytes")
}
```

On the host:

```text
no network
```

The port is 443 unless the URL names another. A failed certificate check
is an ordinary failure: `ok` is false, as for a refused connection or a
timeout. The TLS handshake takes a while on a microcontroller (the
signature checks are spread over many short slices, so other actors keep
running), so give an https request a longer timeout than a plain one.

**Memory.** TLS needs about 23 KB for one connection: two record buffers
(16 KB in, 4 KB out) and the connection state. That memory is charged to
the actor that opens the connection, like any port's state, so it does not
fit the 16 KB default of a board. You do not have to do anything about
it: every request already runs in an actor that `gogo/http` spawns with the
limit it needs (the body limit as before, plus `http.TLSMemory` for https).
Your own actor still needs room for the response body it gets back
(chapter 10).

**Only one at a time.** A board has one TLS connection at most. While one
is open (another request in progress, or a `Client` holding its kept
connection), a second https request fails at once. Plain http requests are
not affected.

**Cut-short responses are errors.** Over TLS, a server's connection close
can be faked by anyone on the path at the right moment, and it then looks
like a normal close. So over https a response must say how long it is: a
`Content-Length`, or chunked encoding, and a body that ends early is a
failure, never a short success. A response that just runs until the
server closes, which plain http accepts, fails over https. That is why an
https `Get` asks in HTTP/1.1 (with `Connection: close`), where a plain one
asks in HTTP/1.0: some servers, Cloudflare among them, answer an HTTP/1.0
request with exactly such a body, and in HTTP/1.1 they frame it.

## 17.3 Keep-alive: `http.Client`

```go
type Client struct{ PID actor.PID }

func NewClient() Client
func ClientDo(c Client, method, url, contentType string, body bytes.Bytes, timeoutMs int) (Response, bool)
func ClientUpload(c Client, url, path, contentType, token string) (status int, err errors.Code)
func CloseClient(c Client)
```

`NewClient` starts an actor that makes requests for you over HTTP/1.1 and
keeps the connection open after each response. The next request to the
same scheme, host and port reuses it: no new TCP connection, no new TLS
handshake. `ClientDo` is like `Do`, with the body limit fixed at
`http.MaxBody`.

```go
// Poll a server once a second over one kept HTTPS connection: one TLS
// handshake for all the requests, not one each. The URL comes from the
// config store (gogo config set poll.url https://...).
package main

import (
	"gogo/actor"
	"gogo/bytes"
	"gogo/dhcp"
	"gogo/http"
	"gogo/sntp"
	"gogo/sys"
)

func main() {
	url, ok := sys.Config("poll.url")
	if !ok {
		url = "https://example.com/status"
	}
	if !dhcp.Start(dhcp.Config{}) || !sntp.Sync("pool.ntp.org", 5000) {
		println("no network")
		return
	}
	c := http.NewClient()
	var none bytes.Bytes
	for i := 0; i < 5; i++ {
		r, ok := http.ClientDo(c, "GET", url, "", none, 5000)
		// A failed request (the server went away mid-request) is not
		// retried for you; the next one reconnects.
		println(i, ok, r.Status, r.Body.Len())
		actor.Sleep(1000)
	}
	http.CloseClient(c)
}
```

On the host:

```text
no network
```

The rules:

- **One connection.** The client keeps at most one. A request to another
  host (or port, or scheme) closes the current connection and opens a new
  one. For two servers, start two clients (only one of them can hold an
  https connection, though).
- **Servers close idle connections**, after a few seconds or minutes. The
  client notices that while it waits and opens a new connection for the
  next request, transparently.
- **No automatic retry.** If the connection ends after a request was sent
  on it, `ClientDo` fails. The server may already have acted on the
  request (a POST that created something), so sending it again is your
  decision, not the library's. The next call opens a new connection.
  There is a short window where the server closes an idle connection just
  as a request goes out; a GET that fails like that is safe to repeat.
- **Responses** are framed by `Content-Length` or by chunked transfer
  encoding (which the client decodes); a response the server marks
  `Connection: close` closes the connection after it.
- **One request at a time.** The client is an actor: requests from several
  actors are answered one after the other, in order. A request whose
  caller gave up while it waited (its timeout passed) is dropped without
  being sent. At most `http.MaxQueued` (4) requests wait at once; one more
  fails at once.
- **It follows its creator.** When the actor that called `NewClient`
  exits or crashes without `CloseClient`, the client closes its
  connection and ends too: a request in progress then (even another
  actor's) fails at once, its connection reset, and the requests queued
  behind it fail at once without being sent. A client does not outlive
  its owner by more than that abort, so a dead owner never keeps the one
  TLS connection.
- **Responses** of type 1xx (`103 Early Hints`) are interim: the client
  skips them and reads the real response that follows.
- **Memory.** The client actor is spawned with the limit an https
  connection and an upload need (60 KB), whatever URL you use. That is a
  limit, not a reservation: it costs memory only while used. Requests
  waiting for it (their bodies too) count against it.
- `CloseClient` closes the connection cleanly and ends the actor; requests
  still waiting for it fail. Close a client you no longer need: while it
  holds an https connection, no other https request can open one.

## 17.4 Uploading a file: `http.Upload`

```go
func Upload(url, path, contentType, token string) (status int, err errors.Code)
```

`Upload` sends the file at `path` as the body of a POST, with its
`Content-Length` and `Content-Type: contentType`. The file is read with
`fs.File.Read`, `http.UploadChunk` (4 KB) at a time, with at most two
writes in flight, so the whole file is never in memory: a 1 MB recording
goes up with the same memory as a 16 KB one. Like `Get`, it runs in an
actor of its own with the limit it needs (46 KB for https), so it
can be called from any actor.

If `token` is not empty, the request carries `Authorization: Bearer
<token>`. **Keep the token out of your source code**: store it on the
device with `gogo config` and read it with `sys.Config`. `gogo/http` never
prints it, and neither should you. A token goes over **https only**: with
an `http://` URL, `Upload` refuses at once with `http.BadURL`, so a mistyped
URL cannot send the token in the clear.

```sh
gogo config set upload.url https://myhost.example.ts.net/upload
gogo config set upload.token -       # type it; it is not echoed back
```

The error is an `errors.Code`, the shared codes of `gogo/errors`
(appendix C). The result is the server's status with `errors.OK` when the
server answered, whatever the status (201, 401, 413...); check it.
Otherwise `status` is 0 and `err` says why:

| `err` | meaning |
|---|---|
| a storage error | the file, with the same number as in `gogo/fs`: `errors.NotFound`, `errors.IsDir`, `errors.IO` (also: the file got shorter while it was sent) |
| `errors.Timeout` (9) | no usable answer in time: a wait or the whole upload ran out |
| `errors.IO` (8) | no usable answer otherwise: DNS, connection, certificate, or a malformed or cut-short response |
| `http.BadURL` (25, `errors.BadURL`) | not an `http://` or `https://` URL, a content type or token that cannot go in a header, or a token with an `http://` URL |

A server that answers before it has read the whole body (a 401 for a bad
token, a 413 for a file too large) stops the upload, and you get its status
without the rest of the file being sent. Each wait (connecting, a write
being acknowledged, the response) has `http.DefaultTimeoutMs`, and the
upload as a whole 30 s plus 1 ms per byte: about 17 minutes for 1 MB, so
only a link slower than about 1 KB/s fails for being slow. A file that grows while it is sent (a recording still
being written) is sent up to the size it had when the upload started.

```go
// Upload a recording with http.Upload: the file is streamed from storage
// 4 KB at a time, never held in memory whole. The URL and the token come
// from the config store, never from the source:
//
//	gogo config set upload.url https://myhost.example.ts.net/upload
//	gogo config set upload.token -     (typed, not echoed)
//
// On the host there is no network, so this shows the error cases.
package main

import (
	"gogo/bytes"
	"gogo/errors"
	"gogo/fs"
	"gogo/http"
	"gogo/sys"
)

func main() {
	url, _ := sys.Config("upload.url")
	token, _ := sys.Config("upload.token") // sent as "Authorization: Bearer <token>"; never print it

	// A 10 KB file to send.
	b := bytes.NewBuffer(10240)
	for i := 0; i < 10240; i++ {
		b.WriteByte(byte(i))
	}
	println("write:", int(fs.WriteFile("/flash/rec.bin", b.Freeze())))

	status, err := http.Upload(url, "/flash/missing.bin", "audio/wav", token)
	println("missing file:", status, int(err), err == errors.NotFound)

	status, err = http.Upload("ftp://myhost/upload", "/flash/rec.bin", "audio/wav", token)
	println("bad URL:", status, int(err), err == http.BadURL)

	// No network on the host: IO. On a board, the server's status
	// (201, 401, ...) with errors.OK.
	status, err = http.Upload(url, "/flash/rec.bin", "audio/wav", token)
	println("upload:", status, int(err), err == errors.IO)
}
```

On the host (the example's `config` file sets `upload.url` and a dummy
`upload.token`; the `dns:` line is the resolver saying there is no
network):

```text
write: 0
missing file: 0 1 true
bad URL: 0 25 true
dns: net.Subscribe failed (no network or 4 subscribers): retrying on each lookup
upload: 0 8 true
```

`Upload` runs in an actor of its own that follows the caller: if the
caller crashes or a supervisor kills it mid-upload, the upload stops and
its connection is reset at once, so the restarted caller's retry finds
the TLS connection free (and the server never gets a duplicate finished
behind its back).

`Upload` uses a connection of its own (HTTP/1.1, `Connection: close`). On
a board that means it needs the one TLS connection: while an https
`Client` holds it, the upload fails. If the program has a client to the
same server anyway (a status poll once a second), upload through it:
`http.ClientUpload(c, url, path, contentType, token)` sends the same
request on the client's kept connection, with the same results, and keeps
the connection for the next request.

## 17.5 The `"tls"` port

`gogo/http` sits on a port you can also use directly, for a protocol of
your own over TLS. It is the `"tcp"` port with TLS in between, and has the
same messages (`Connected`, `Shutdown` and the stream protocol's
`port.Out`, `Done`, `Chunk` and `Closed`, section 7.6); only its config
differs:

```go
c, ok := port.Open("tls", net.DialTLS{IP: ip, Port: 443, TimeoutMs: 15000, Host: "myhost.example.ts.net"})
```

`Host` is the name the certificate must be for (it is also sent to the
server, SNI). `Connected` comes after the handshake. `Closed` has three
more reasons: `net.NoTime` (the clock is not set; nothing was sent),
`net.BadCert` (the certificate is untrusted, expired or for another name)
and `net.TLS` (any other TLS failure). The owner needs about 23 KB plus
room for one `Chunk` beyond its own heap, or the open fails: spawn it with
`actor.SpawnOpt` and a larger `MaxMemory`, as `gogo/http` does. And frame
what you read (a length, a terminator): a close at a record boundary looks
normal, so "read until the close" cannot tell a whole stream from a cut
one. The full contract is in the `DialTLS` comment in `lib/net/net.gg`.

## 17.6 On the host, and requirements

`gogovm` built with mbedTLS (the Makefile does it when it finds the
ESP-IDF source) has the `"tls"` port. It trusts the host's CA bundle, or
the certificates in the file named by `$GOGO_TLS_CA_FILE`. The tests in
`tests/net` use that to run `gogo/http` against real Go TLS servers with a
test CA, through the network simulator: https, wrong names, keep-alive,
idle closes, cut-short responses and a 1 MB upload.

A program that opens `"tls"` itself requires the `tls` driver, and a
firmware built without it (`gogo firmware` feature `tls`, on by default)
refuses the image. Importing `gogo/http` does not require it: the library
opens `"tls"` only for an https URL, and without it that request just
fails (one-shot and on a Client alike). It says so with the directive
`//gogo:optional tls` (chapter 12), which any library can use for its own
soft drivers. So an http-only program runs on any firmware with
networking.

## 17.7 On the board

`examples/httpsdemo` is the whole chapter on a StickS3: DHCP, `sntp.Sync`,
one `http.Get` of `https://example.com/`, three requests through a
`Client`, and a connection with the wrong name that must be refused:

```text
sntp: ok, 50 ms unix ms 1795628156793
httpsdemo: 15 s 后开始 / TLS in 15 s
Get HTTP 200 body 577 B, 2790 ms
Client 1 HTTP 200 body 577 B, 2887 ms
Client 2 HTTP 200 body 577 B, 316 ms
Client 3 HTTP 200 body 577 B, 312 ms
wrong.example: closed 23 , 1085 ms
httpsdemo: done
```

What the numbers say:

- **A handshake takes about 2.5 s.** Almost all of it is elliptic-curve
  arithmetic in software: checking the certificate chain (example.com's
  has a P-384 root) takes about 1.8 s, the server's key-exchange signature
  about 0.35 s. These checks are cut into slices of about 10 to 25 ms
  (a few that cannot be cut, near 50 ms), so other actors keep running.
  Give https requests a timeout of 10 s or more.
- **Keep-alive pays once.** `Client 1` includes the handshake; `Client 2`
  and `Client 3` reuse the connection and take about 0.3 s, the network
  round trip.
- **One step is not sliced.** The key exchange (X25519, the faster curve
  here) is a single step of about 200 ms. For that long nothing else runs:
  no other actor, no timer. A program that must not pause that long (audio
  to SD, say) should not start an https request while it is busy. Making
  this step sliced too is on the list (`docs/known-issues.md`).
- **The wrong name is refused.** Here the server itself refuses a name it
  does not serve (`Closed{TLS}`, reason 23); a server that answers with its
  certificate anyway gets `Closed{BadCert}` (21) from the name check.

The TLS memory comes from PSRAM on the StickS3 (about 35 KB at its peak,
during the handshake), so the internal RAM the VM uses is not touched;
on the Cardputer ADV, which has no PSRAM, it comes from internal RAM. The
board keeps the trust anchors in flash: ESP-IDF's certificate bundle.
`gogo config stats` shows the TLS counters (`tls_*`), the longest slice
(`tls_max_step_us`) and the TLS memory (`mbedtls_mem_peak`).

## 17.8 Limits

| | board | host |
|---|---|---|
| TLS connections at once | 1 | 1 |
| TLS version | 1.2 | 1.2 |
| memory charged per TLS connection | about 23 KB (+ one `Data`) | same |
| `Client` connections | 1 per client | same |
| `Client` / `Upload` actor limit (https) | 60 KB / 46 KB | same |
| requests waiting for a `Client` | 4 (`http.MaxQueued`) | same |
| upload chunk, writes in flight | 4 KB, 2 | same |
| response body | `http.MaxBody` (8 KB; up to 64 KB with `Do`) | same |

Responses are still read into memory whole, up to the body limit;
downloading a large file to storage in a stream is not there yet.
