# nodehello: distributed actors on one computer

The smallest `gogo/node` program: a **server** node exports a counter actor
as `"counter"`, and a **client** node on the host finds it, calls it five
times and notices when the server goes away. One program plays both parts,
so both nodes share the `Add` message type (book, section 20.4).

## Run

Both nodes need the same token. Put it in the config file the host VM
reads (`~/.gogo/config`, or the file named by `GOGO_CONFIG`):

```sh
printf 'node.token=<a long random string>\n' >> ~/.gogo/config
go build -o build/gogo ./cmd/gogo

# terminal 1 (port 4370 unless given: server <port>)
GOGO_NET=host build/gogo run examples/nodehello server

# terminal 2 (127.0.0.1 by default; give the server's ip[:port] from another machine)
GOGO_NET=host build/gogo run examples/nodehello client
```

`GOGO_NET=host` gives the host VM the computer's real sockets (without it,
the `tcp` port is the simulated network of the tests).

The client prints:

```text
client: found counter <pid 4@1>
added 1 total 1
added 2 total 3
added 3 total 6
added 4 total 10
added 5 total 15
client: done
```

and the server logs each `Add` with the client's PID (`<pid 0@2>`: actor 0
on node 2). Stop the server while the client runs and the client's monitor
fires at once:

```text
server gone, reason 11
```

Reason 11 is `actor.NoConnection`: the node holding the actor became
unreachable.

## What it shows

| Step | Call |
|---|---|
| start a server node | `node.Listen(port, "server")` |
| make an actor reachable | `node.Register("counter", pid)` |
| join as a client | `node.Connect(addr, "client")` |
| find a remote actor | `node.Whereis("server", "counter", ms)` |
| request and reply | `actor.Call(counter, Add{N: i}, ms)` / `actor.Reply` |
| notice the node going away | `actor.Monitor(counter)`, then `actor.DownMsg` with `NoConnection` |

A board joins the same way: replace the client's start with
`dhcp.Start(...)` and `node.Connect("<server ip>:4370", "kitchen")`. The
snake (`examples/snake`, `snake.server`) is a larger example.
`tests/dist` (`TestNodeHello`) runs this one: a server, a client to the
end, and a second client that sees the server killed.
