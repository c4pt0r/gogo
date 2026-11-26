# 20. Distributed Actors

An actor on one board can talk to an actor on another board, or on a
server, with the same `actor.Send`, `actor.Call`, `actor.Monitor` and
`actor.Link` it uses at home. The library is `gogo/node`.

The shape is a **star**: one **server node** (a gogo VM on a computer,
with real sockets) and any number of **boards** that connect to it over
TCP. The server relays messages between boards, so a board needs one
connection whatever it talks to.

```text
   board "cardputer" ---\                      /--- board "sticks3"
                         >--- server node ----<
   board "kitchen" -----/    (gogovm --host-net)
```

## 20.1 Remote PIDs

A remote actor's PID is an ordinary `actor.PID`. You get one from
`node.Whereis`, or in a message (a `Request`'s `From`, a struct field),
and then use it like any other:

| Operation | On a remote PID |
|---|---|
| `actor.Send(p, m)` | the message is encoded, sent over the link and delivered to `p`'s mailbox |
| `actor.Call(p, req, ms)` / `actor.Reply` | work unchanged (the reply comes back over the link) |
| `actor.Monitor(p)` | `DownMsg` when `p` exits (its reason code), or `actor.NoConnection` when its node becomes unreachable |
| `actor.Link(p)` / `Unlink` | exit signals both ways; `NoConnection` when the node is lost |
| `==`, type switches | as for local PIDs: one remote actor has one PID value on this node |
| `println(p)` | `<pid 5@2>`: actor slot 5 on node 2 |
| `actor.Kill(p, code)` | an exit signal on `p`'s node, as for a local actor (`p` must have been given out by its node; a node's door cannot be killed, nor linked to) |
| `actor.SendAfter(ms, p, m)` | not across nodes: the sender exits with `actor.BadMsg` |
| `actor.Info`, `actor.Register` | Info reports 0; Register refuses a remote PID |

A PID of this node that comes back from another node is the local PID
again, so `actor.Self()` sent away and back is `== actor.Self()`.

## 20.2 A server node

The server is a gogo program on a host VM that uses the host's sockets:
`gogovm --host-net`, or `GOGO_NET=host` in the environment, which also
works through `gogo run`. (Without it the host VM's `tcp` port is the
simulated network of the tests, chapter 12.) With it, a program gets the
host's real network: `tcp_listen` listens on every interface of the
computer, and `tcp` may connect anywhere. Do not export `GOGO_NET=host`
in a shell profile: every `gogo run` would get real sockets.

```go
func main() {
	if err := node.Listen(4370, "server"); err != nil {
		println("cannot listen:", err.Error())
		return
	}
	node.Register("echo", actor.Spawn(echo))
	...
}
```

```sh
printf 'node.token=<a long random string>\n' >> ~/.gogo/config
GOGO_NET=host build/gogo run server/
```

`node.Listen(port, name)` starts the node and returns at once; boards may
join from then on. Its error tells why not, in the codes of `gogo/errors`
(3.10, appendix C): `errors.BadArg` without a token, `errors.Unsupported`
without the host network, `errors.Busy` if the port is taken or the VM is
a node already. `node.Register(name, pid)` makes an actor reachable
from other nodes under a name. Names registered with `actor.Register` stay
local: another node can find only what you `node.Register` (which
returns `errors.Exists` for a name already taken).

## 20.3 Joining from a board

A board needs an address first (`gogo/dhcp`), then joins:

```go
dhcp.Start(dhcp.Config{})
if err := node.Connect("192.168.1.10:4370", "kitchen"); err != nil {
	println("no server:", err.Error())
	return
}
echo, err := node.Whereis("server", "echo", 2000)
if err != nil {
	return
}
r, ok := actor.Call(echo, "hi", 2000)
```

`node.Connect(addr, name)` dials the server (a numeric `a.b.c.d:port`; the
port defaults to `node.DefaultPort`, 4370), proves the shared token, and
returns `nil` once the board is a node. Otherwise its error says why:

| Error | Why |
|---|---|
| `errors.BadArg` | no token or name, or `addr` is not `a.b.c.d[:port]` |
| `errors.Busy` | this VM is a node already |
| `errors.Unsupported` | the firmware has no network |
| `errors.Refused`, `errors.NoRoute`, `errors.Timeout`, ... | the TCP connection failed (the `tcp` port's reason) |
| `errors.Refused` | the server rejected the handshake (another token) |
| `errors.Timeout` | the handshake did not finish in time |

`node.Whereis(node, name, timeoutMs)` asks that node for an exported
name; it works for the server, for other boards (through the relay), and
for the board itself. It fails with `errors.NotFound` (no such node or
name), `errors.Closed` (this VM is not a node) or `errors.Timeout`.

Both ends need the same **token**: the config key `node.token` (`gogo
config set node.token ...`), or `node.Options{Token: ...}` with
`ConnectOpt` / `ListenOpt`. Without one, `Connect` and `Listen` return
`errors.BadArg`. `node.name` in the config can give the node's name too.

`node.Nodes()` lists the nodes this one can reach and `node.Self()` is its
own name. `node.Disconnect()` drops a board's link.

## 20.4 What a message may hold

A message to a remote PID is encoded on the sender's node and rebuilt on
the receiver's. What may cross:

| Value | Crosses |
|---|---|
| `int`, `int64`, `float32`, `float64`, `bool`, `string`, `bytes.Bytes`, `[]byte`, slices, `nil` | yes (floats bit for bit) |
| actor PIDs, Refs | yes |
| the system structs (`actor.Request`, `port.Chunk`, `net.Addr`, ...) | yes |
| your own structs | yes, by name: the receiver must have a struct of the same qualified name (`snake/board.View`, `main.Ping`), the same number of fields and the same field kinds (int, string, PID, a slice of int, ...); otherwise the receiver drops the message |
| your own named non-struct types in an interface (`any(Dir(3))` for `type Dir int`) | yes, by name, as your own structs |
| func values, `bytes.Buffer`, pointers and maps, a port's PID | no: the sender exits with `actor.BadMsg` |
| a Ref made by the receiving node itself | no: the receiver drops the message (a peer could otherwise forge a reply or a `DownMsg`) |
| a value nested deeper than 16, or more than about 4 KB encoded | no: `actor.BadMsg` |

The simplest way to share message types is a package that both programs
import (chapter 3.1), or one program that plays both parts, as the snake
does (20.8). Two programs that both declare `type Ping struct{ From
actor.PID; N int }` in `package main` share `main.Ping` too.

Two differences from a local send: values that share a slice inside one
message arrive as separate copies (local sends keep the sharing), and a
`[]any` holding itself fails with `BadMsg` instead of being copied.

## 20.5 When a node goes away

Links and monitors cover remote actors exactly as local ones, with one
new reason, `actor.NoConnection` (11): the node holding the actor became
unreachable (its TCP connection ended, or the server reported it gone).

```go
ref := actor.Monitor(remote)
...
case actor.DownMsg:
	if m.Ref == ref && m.Reason.Code == actor.NoConnection {
		// the server is gone: carry on locally
	}
```

A board that loses the server loses every node at once. Monitoring or
linking a PID whose node is already gone gets the `DownMsg` or exit
signal at once, as for a dead local actor (with `NoConnection`). A
`Killed` exit reaches a linked actor that traps exits as an `ExitMsg`,
as locally.

`actor.Call` to a PID whose node is gone returns `(nil, false)` at once,
as a Call to a dead local actor does, and so does a Call waiting for its
reply when the node goes. A node tracks a limited number of remote Calls
in flight (20.9); a Call beyond that returns `(nil, false)` at once
rather than risk waiting out its timeout.

`node.Subscribe()` makes the caller receive `node.Up{Node}` and
`node.Down{Node}` for every node that joins or goes. A board that wants
to stay joined connects again after a `Down` of the server:

```go
node.Subscribe()
for {
	if node.Connect(addr, "kitchen") == nil {
		waitForServerDown()
	}
	actor.Sleep(3000)
}
```

## 20.6 Delivery

- **At most once.** A message is never duplicated. It may be lost: when
  the link is full, when the receiver does not have its struct type, or
  when it would take the receiver over its memory limit. A `Call` whose
  request or reply is lost that way waits for its timeout (a Call to a
  node that is gone fails at once, 20.5).
- **In order** between one sender and one receiver, relayed or not.
- **Backpressure.** Each connection's link actor sends frames to TCP at
  TCP's pace (the stream protocol's `Out` and `Done`, chapter 10.7) and
  queues up to 8 KB more; beyond that, messages are dropped and the link
  stays up. Senders never block. Unlike a local send, a remote message that would take its
  receiver over its memory limit is dropped: a peer cannot end a local
  actor by sending it data.

## 20.7 Security

- **Authentication.** Both ends prove the token with a challenge and
  HMAC-SHA-256; the token itself never crosses the network. A node with
  the wrong token is refused.
- **What a peer can reach.** A node's zero PID is its *door*, which only
  answers `node.Whereis` for names exported with `node.Register`; the
  node's agent, which keeps the names and the node table, is never
  reachable from outside. Besides the door, a peer can send to the PIDs
  this node gave out in a message to *any* peer (a `Request`'s `From`, a
  field): the list is per node, not per peer, and a PID once seen by one
  peer may be passed to another. A PID the node never gave out reaches
  nothing; link, monitor and kill signals need a PID given out, and exit
  and down signals are accepted only for links and monitors that exist.
- **Integrity.** Both ends derive a session key from the token and the
  handshake, and every frame carries its number and a MAC (HMAC-SHA-256)
  over both: a frame changed or injected on the way ends the link, and a
  replayed or reordered one is refused.
- **Not encrypted.** The messages travel in plain TCP; someone on the
  network can read them. Use a trusted network and a long random token
  (a captured handshake allows guessing a weak one offline). TLS for node
  links is not there yet (known issues).
- **Unauthenticated connections** may hold at most two handshakes at a
  time on a server; more are closed at once, and a handshake that does not
  finish within its timeout (5 s) ends.
- All nodes with the token are trusted alike.

## 20.8 Example: the snake's autopilot on a server

[examples/snake](../../examples/snake/README.md) runs its autopilot on a
server node when the config key `snake.server` is set: the game on the
board sends the server's autopilot a `board.View` after every step and
gets a `board.Move` back, one request and one reply per step over Wi-Fi.
The game monitors the remote autopilot; when the link drops, the
`DownMsg` (reason `NoConnection`) makes it start a local autopilot at
once, and a joiner actor connects again when the server is back. The
server is the same program with `snake.serve` on a host VM.

On a StickS3 over Wi-Fi to a Mac on the same network, a `View` and its
`Move` took 7 ms at best and 51 ms on average (50 round trips; Wi-Fi
delays a few by several hundred ms).

## 20.9 Limits

| Limit | Value |
|---|---|
| frame (one message, encoded) | 4096 bytes |
| node links per VM, *L* | 8 on a board, 512 on a host VM; set with the config key `node.max_links` or `gogovm --max-links` (at most 2048) |
| ports per VM | 16 on a board, 1024 on a host VM; `vm.max_ports` or `gogovm --max-ports` (at most 2048) |
| ports per link | 1 on a host VM (the node port takes the TCP connection over), 2 on a board (TCP and node port) |
| remote PIDs known at once | 48 + 2*L* (64 on a board; also at most 4096 minus the port cap) |
| local PIDs given out | 48 + 2*L* (dead ones are reused) |
| remote links and monitors | 16 + 2*L* (a full table answers with `NoConnection`) |
| remote Calls in flight | 8 + *L* (a Call beyond that returns `(nil, false)` at once) |
| nesting of a message | 16 |
| queued frames per link | 8 KB |

**How many boards a server serves** is set by its link and port caps: a
host VM serves up to 512 boards by default, each costing it one port, so
the defaults (1024 ports, 512 links) leave room for the program's own
ports; raise both, up to 2048, for more. The tables above are allocated at
the first link, about 100 bytes per link of the cap (some 50 KB at 512),
plus about 300 bytes per open link and its link actor's heap (at most
40 KB, usually a few). A board joins one server, so its 8 links are
plenty.

**Full tables.** When the remote-PID table is full, the least recently
used entry that no link or monitor names makes room. A remote PID that an
actor kept without monitoring it can so go *stale*: sends to it are
dropped, and it is no longer `==` a fresh copy of the same PID from a new
message. Monitor the remote PIDs you keep (a subscriber list on the
server, say): a monitored PID is never evicted. When the link's queue is
full, the message is dropped (counted), and the link stays up: frames
carry their number, and the receiver accepts gaps (20.6).

The VM core gains only the hooks; the rest is the `"node"` port
(`runtime/dist/`) and the `gogo/node` library. The programs of this
chapter are tested as separate VMs on one machine by `tests/dist`
(`go test ./tests/dist/`).
