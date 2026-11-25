# hotload_net: hot loading over Wi-Fi

The board fetches `worker_v1.gmod` over HTTP, loads it, runs it under
`supervisor.SuperviseModule`, then fetches `worker_v2.gmod` and upgrades the
running worker. It prints each step, the fetch and load time in ms, and
`code mem` (`code.MemUsed`). The modules are those of `examples/hotload`.

On a StickS3 the code is copied to PSRAM; on a Cardputer ADV it runs from the
received bytes in place or from a copy.

```sh
go build -o build/gogo ./cmd/gogo
mkdir -p /tmp/mods
build/gogo build examples/hotload/mods/worker_v1 -o /tmp/mods/worker_v1.gmod
build/gogo build examples/hotload/mods/worker_v2 -o /tmp/mods/worker_v2.gmod
(cd /tmp/mods && python3 -m http.server 8000)          # in another terminal

gogo config set wifi.ssid <name>
gogo config set wifi.password -                         # reads stdin
gogo config set demo.modurl http://<mac-lan-ip>:8000    # base URL, no trailing /
build/gogo build examples/hotload_net -o hotload_net.gogo
build/gogo flash hotload_net.gogo
build/gogo monitor -reset -t 40 -until "done"
```

The last line is `done` on success or `FAIL: <reason>`; the monitor ends
at `done` or after 40 s.

Use the Mac's LAN address (not localhost). Expected output (times vary):

```
step 1: network
network up; code mem 0
step 2: fetch and load v1
fetched worker_v1.gmod <n> bytes in <ms> ms; loaded in <ms> ms; code mem <n>
step 3: v1 running
worker v1: message 1 ...
worker restarted from the module
step 4: waiting 5 s, then upgrade
fetched worker_v2.gmod ...
upgrade result (0 is ok): 0
step 5: v2 running; code mem <n>
done        (or: FAIL: <reason>)
```
