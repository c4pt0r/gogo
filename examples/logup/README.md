# logup — the board uploads its log to your computer, as it happens

One program on the board, one on the computer. The board turns what it
senses into log lines and POSTs them in batches; the computer prints each
line with the moment it landed, and can keep them in a file.

```
computer (Linux, macOS)
-----------------------
go run ./examples/logup/host -addr :8080 -out logs

board (Cardputer ADV, M5StickS3)
--------------------------------
gogo config set logup.url 'http://<computer-ip>:8080/ingest?board=cardputer'
gogo config set wifi.ssid <name>
gogo config set wifi.password -
gogo flash examples/logup
```

The computer's address is its LAN address (`ip -4 addr show` on Linux,
`ipconfig getifaddr en0` on macOS, usually the Wi-Fi); the board must be on the same network.

No network? The board prints every line anyway, so the USB cable is the
transport:

```sh
gogo monitor -reset -timeout 60 | go run ./examples/logup/host -stdin
```

## What gets logged

Each source is an actor that only sends lines. A source whose port refuses
to open logs *that* and stops — the collector gets the reason, which is what
a device log is for.

| source | line | config |
|---|---|---|
| `imu` | `imu samples=20 gx=0 gy=0 gz=0 ax=11 ay=766 az=636 dropped=0` (milli-g, deg/s) | `logup.src` |
| `kbd` | `key down=1 code=44 char=115 mods=0` (USB HID usage; both edges) | `logup.src` |
| `mic` | `mic samples=1600 peak=90 mean=8 dropped=0` (a level, not audio) | `logup.src` |
| net | `net up`, `net down`, `net: no netif port (…)` | always |
| beat | `beat t=12034` — a quiet board still has to prove it is alive | `logup.beat` ms, 5000, 0 off |

Upload and backlog:

| key | default | meaning |
|---|---|---|
| `logup.url` | none | collector address; unset means print only |
| `logup.batch` | 4 | lines per POST (one keep-alive connection, one request per batch) |
| `logup.max` | 48 | lines kept while the upload fails; the oldest are dropped and counted (a `logup.batch` over it is lowered to it) |
| `logup.attempts` | 3 | POSTs per batch; a `net up` gives the waiting batch its attempts back |
| `logup.retry` | 1000 | ms before the first retry, doubling each attempt |
| `logup.retrymax` | 30000 | the most a retry ever waits |
| `logup.timeout` | 3000 | ms per POST |
| `logup.sim` | 0 | N: write N lines of its own and stop (the golden; also tries a collector with no board) |

## Where the collected log lives

`-out DIR` writes one file per board per day, `DIR/<board>-<YYYY-MM-DD>.log`,
appended to, each line prefixed with the moment the collector received it
(`2026-11-26 13:25:38.038 imu samples=20 ...` — the board's clock never has to
agree with yours). Without `-out` the collector only prints. It keeps no
rotation and no retention: point it at a directory you clean up, or run it
under a supervisor. After about an hour of one board:

```
/tmp/gologs/cardputer-2026-11-26.log    880 lines, 64 KB   (appends across collector restarts)
```

## What an outage looks like, measured

The collector was stopped for 18 s and restarted. From the board's own
output (`gogo monitor`, 46 s):

```
# logup: uploading to http://192.168.1.16:8080/ingest?board=cardputer (batch 8, max 20, attempts 1000)
# backlog full: dropped 8 lines, keeping the newest 20
...
net up                                    # Wi-Fi had not associated yet at boot
# delivered 8 lines (HTTP 200)          x4      the backlog draining
# send failed: I/O error               x169    the collector was down
# delivered 8 lines (HTTP 200)          x25     it came back
```

Two things the program does so a board left running for a week behaves:
nothing is POSTed while there is no link (a POST with no link costs radio
time and nothing else — the backlog keeps the newest `logup.max` lines
instead), and retries back off. The backoff is a *deadline* (`nextTry =
now + logup.retry`, doubling to `logup.retrymax`), not only a timer message:
an earlier version also retried from the path that runs for every new log
line, so the retry rate was the log rate — 169 failed posts in the 35 s
above, about one per new line, instead of one per `logup.retry`. A
`net up` clears the deadline and drains the backlog at once.

## Why the actors are shaped this way

The sources send lines and keep nothing. The uploader owns the HTTP client
and keeps nothing: the batch travels *in the request*, and the answer is a
message the collector addressed to itself — not `actor.Call`, which would
stop the collector for the whole POST and let a slow link stall the log. The
collector holds all the state (the batch on its way out, the backlog, the
counters) and monitors the uploader, so an uploader that dies costs a
counter and a retry, not the log.

When the log is bigger than the link, the newest lines win and the dropped
count says how many were lost — a log you know is missing 40 lines beats one
that quietly fills up memory.

## What the golden covers

`make test-examples` runs it with `logup.sim=12` and a `logup.url` the host
VM has no link to, so it checks the line format, the batching, the failed
posts, the backlog keeping its newest lines, the drop count and the closing
tally — byte for byte, with no timing in it (`config`, `expected.out`). The
HTTP client path it uses is the one `tests/net/http_test.go` covers against
the network simulator (`TestHTTPPost`).

## On a real Cardputer ADV (measured, firmware `v0.1.0-387-g9787f4a`)

Serial capture, no network (`gogo monitor | host -stdin`), 14 s:

```
13:17:34.230  cardputer   mic samples=1600 peak=4051 mean=279 dropped=0
13:17:34.232  cardputer   imu samples=20 gx=62 gy=5 gz=4 ax=18 ay=711 az=672 dropped=0
13:17:34.438  cardputer   mic samples=1600 peak=532 mean=100 dropped=0
13:17:34.440  cardputer   imu samples=20 gx=-4 gy=0 gz=0 ax=13 ay=769 az=640 dropped=0
```

Ports this board gives a program, from a port-by-port probe: `kbd`, `mic`,
`speaker`, `netif`, `power` open; `ir` answers `not found` (the ADV has IR
send only, `HAS_IR_RX 0` in `board.h`); `screen` opens with its default
grid. Three rough edges, all visible in the log rather than hidden:

- `port.Open("imu", …)` came back `busy` on **1 of 4** boots (the BMI270
  bring-up: `imu_esp.c` probes `0x69`/`0x68` and uploads its config file; a
  failed open is redone on the next attempt). The example logs `imu: unavailable
  (busy)` and carries on without motion data.
- Repeated `gogo monitor -reset` makes the access point refuse the board
  for a while (`wifi: Association refused too many times` / `reason 208`),
  and the first upload after a reset can then wait 15 s. That is the AP, not
  gogo: leave a gap between resets.
- A driver's refusal that comes back as a negative number is reported as
  `busy`, whatever it meant (book 22.6): `screen_port_open` returns `-1` for
  a grid that is too big as well as for no hardware, so
  `screen.Config{Cols: 40, Rows: 12}` on a board whose console grid is 40×11
  (`console_task.c`) fails with `busy`, while `screen.Config{}` (the
  defaults) opens fine.

## Limits, left as they are (it is a demo)

Measured with `logup.max` set far too high and 4000 lines pushed at once:
the real ceiling is not `logup.max` but the collector's own mailbox and heap
— **172 lines** on the board configuration (`gogovm_mcu`, 4096 words per
actor) and 2296 on `gogovm` — and what happens past it is
`actor <pid 1> crashed: mailbox_overflow`. Queued messages count against
their receiver (design notes, decision 5), so trimming `logup.max` does not
protect against a producer that outruns the post: the collector dies, nothing
restarts it, and the uploads stop without saying so.

Raising the ceiling is a few lines each, not a rewrite: spawn the collector
with `actor.Options{MaxMemory}` and clamp `logup.max` to what fits; supervise
it with `gogo/supervisor`; spill the backlog to `/flash` with `gogo/ringlog`
(~55 bytes a line, so 512 KB is ~8500 lines, and it survives a reboot);
throttle the sources with one ack per line. And note the drop count is a
serial-side status line: a collector that cannot be reached sees a gap in its
file with nothing marking the gap. All of that is out of scope here on
purpose.
