# lib: the gogo standard library

gogo sources (`.gg`, the Go subset) of every `gogo/...` package, one
directory per package: import path `gogo/X` is `X/` here (`gogo/net` is
`net/`; the C network stack is `runtime/net/`). `embed.go` (package
`gogotools/lib` of the repository's root module `gogotools`, the only Go
file here) embeds `*/*.gg` into the compiler with `//go:embed`.

Editors: map `.gg` to Go for highlighting (book chapter 2, *Editor
support*). The Go toolchain and gopls do not read `.gg` files.

Host checks (`make test-lib`, part of `make test` through `go test ./...`):
`tests/lib_test.go` copies every `X/*.gg` as `X/*.go` into a temporary Go
module `gogo`, where every package type-checks on the host (stub bodies are
`panic("gogo only")`), with the host tests (`X/*_test.go`, real Go with
build tag `gogohost`: `irproto/irproto_test.go`), and runs `go vet
-stdmethods=false ./...` (`bytes.Buffer.WriteByte` deliberately returns no
error) and `go test ./...` there.

Two kinds of package:

- **stub**: API only. The bodies are never executed: the compiler replaces
  every call with a VM instruction or builtin, and its struct types have
  fixed type indices (`TYPE_*` in `runtime/vm/opcodes.h`). The list is
  `isStub` in `compiler/load.go`.
- **library**: written in the gogo subset and compiled into every image that
  imports it (no dead-function elimination). A library may import stubs and
  other libraries, without cycles.

The requirements column lists the names the compiler records in the
image when a package is compiled in (`compiler/modinfo.go`): the driver of
each constant `port.Open` in the package, and `/sd` for a constant `/sd`
path given to `gogo/fs`. An import by itself requires nothing. The
firmware refuses an image that requires a driver or mount it lacks
(`code.Missing` names it). Packages built only on others inherit their
names (for example `gogo/http` via `gogo/dns`).

A library whose port drivers are soft (opened only when a call asks for
them at run time, their absence reported as that call's failure) names
them in a directive line, conventionally the last line of its package doc
comment after an empty `//` line:

```go
//gogo:optional tls
```

Those drivers then add no requirement for the package's **own** constant
`port.Open` calls; `package main`, importers and other libraries still
count theirs (`package main` may use the directive too, for its own
calls). Any driver name is accepted (a board's or a host port's); an
empty list or a misspelt `//gogo:` directive is a compile error
(`directives` in `compiler/load.go`).
`gogo/http` declares `tls`, `gogo/sound` declares `mic speaker`, `gogo/tui`
declares `kbd`. Book chapter
12 has the details.

| package | kind | imports | requires |
|---------|------|---------|------------|
| `gogo/actor` | stub | | none |
| `gogo/port` | stub | actor, bytes | none (each constant `port.Open`: its driver); also the stream protocol every byte port speaks (`Chunk`, `Out`, `Done`, `Closed`) and the host-port types |
| `gogo/bytes` | stub | | none |
| `gogo/net` | stub | actor, bytes | none (the drivers it is used with: `netif`, `udp`, `tcp`, `tcp_listen`, `tls`) |
| `gogo/gpio` | stub | | none (the `gpio` port: `gpio`) |
| `gogo/fs` | stub | actor, bytes | none (constant `/sd` paths: `/sd`) |
| `gogo/kbd` | stub | | none (the `kbd` port: `kbd`) |
| `gogo/power` | stub | | none (the `power` port: `power`) |
| `gogo/code` | stub | actor, bytes | none |
| `gogo/imu` | stub | | none (the `imu` port: `imu`) |
| `gogo/ir` | stub | | none (the `ir_tx` / `ir_rx` ports: `ir_tx` / `ir_rx`) |
| `gogo/audio` | stub | | none (the `mic` / `speaker` ports: `mic` / `speaker`) |
| `gogo/sys` | stub | | none (the clock, randomness and `sys.Config`, the configuration store) |
| `gogo/screen` | stub | | none (the `screen` port: `screen`) |
| `gogo/supervisor` | library | actor, code | none |
| `gogo/strings` | library | | none |
| `gogo/strconv` | library | | none |
| `gogo/dhcp` | library | actor, bytes, net, port, sys | `netif`, `udp` |
| `gogo/dns` | library | actor, bytes, net, port | `udp` |
| `gogo/http` | library | actor, bytes, dns, fs, net, port, strconv, strings | `tcp`, `tcp_listen`, `udp` (opens the `tls` port for `https://` URLs without requiring it: `//gogo:optional tls`) |
| `gogo/sntp` | library | actor, bytes, dns, net, port, sys | `udp` |
| `gogo/kv` | library | bytes, fs | none |
| `gogo/ringlog` | library | bytes, fs | none |
| `gogo/imulog` | library | actor, bytes, fs, imu, port | `imu` |
| `gogo/wav` | library | bytes, fs | none |
| `gogo/sound` | library | actor, audio, fs, port, wav | none (opens `mic` for Record and `speaker` for Play without requiring them: `//gogo:optional mic speaker`) |
| `gogo/irproto` | library | bytes | none |
| `gogo/tui` | library | actor, bytes, gpio, kbd, port, screen, strconv | `gpio`, `screen` (opens `kbd` without requiring it: `//gogo:optional kbd`) |
