# hotload_net: hot loading over Wi-Fi

The board fetches `worker_v1.ggb` over HTTPS, loads it, runs it under
`supervisor.SuperviseModule`, then fetches `worker_v2.ggb` and upgrades the
running worker. It prints each step, the fetch and load time in ms, and
`code mem` (`code.MemUsed`). The modules are those of `examples/hotload`.

> **Load only modules you trust.** A loaded module runs with the same
> rights as the main program: every port, the config store, the files and
> the network. Modules are not signed, and nothing in a module says who
> built it, so the channel it comes over is the only proof of where it
> comes from. That is why the example takes only an `https://` base URL,
> whose server must have a certificate the board trusts, and refuses
> `http://`: over plain HTTP anyone on the path (the Wi-Fi, the router)
> can swap the module for their own code. Keep the server, and who can
> write the modules on it, as safe as the firmware itself.

On a StickS3 the code is copied to PSRAM; on a Cardputer ADV it runs from the
received bytes in place or from a copy.

The firmware needs `net` and `tls` (the release firmware has both). The
board checks the server's certificate against the public authorities it
trusts (chapter 17 of the book), so the modules must come from a server
with a certificate from a public CA such as Let's Encrypt; a self-signed
one is refused. One way from your computer is Tailscale Funnel, which
publishes a local server under `https://<machine>.<tailnet>.ts.net` with
such a certificate:

```sh
go build -o build/gogo ./cmd/gogo
mkdir -p /tmp/mods
build/gogo build examples/hotload/mods/worker_v1 -o /tmp/mods/worker_v1.ggb
build/gogo build examples/hotload/mods/worker_v2 -o /tmp/mods/worker_v2.ggb
(cd /tmp/mods && python3 -m http.server --bind 127.0.0.1 8000)   # in another terminal
tailscale funnel 8000                                             # in a third; prints the https URL

gogo config set wifi.ssid <name>
gogo config set wifi.password -                         # reads stdin
gogo config set demo.modurl https://<machine>.<tailnet>.ts.net   # base URL, no trailing /
build/gogo build examples/hotload_net -o hotload_net.ggb
build/gogo flash hotload_net.ggb
build/gogo monitor -reset -timeout 60 -until "done"
```

Stop the funnel when you are done: while it runs, anyone can read the
modules. Any other https server the board trusts works the same way.

The last line is `done` on success or `FAIL: <reason>`; the monitor ends
at `done` or after 60 s. Each fetch is a TLS handshake, which takes a few
seconds on the board.

Expected output (times vary):

```
step 1: network
network up; code mem 0
step 2: fetch and load v1
fetched worker_v1.ggb <n> bytes in <ms> ms; loaded in <ms> ms; code mem <n>
step 3: v1 running
worker v1: message 1 ...
worker restarted from the module
step 4: waiting 5 s, then upgrade
fetched worker_v2.ggb ...
upgrade result (0 is ok): 0
step 5: v2 running; code mem <n>
done        (or: FAIL: <reason>)
```
