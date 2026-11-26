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
module `gogo`, where every package type-checks on the host (builtin bodies are
`panic("gogo only")`), with the host tests (`X/*_test.go`, real Go with
build tag `gogohost`: `irproto/irproto_test.go`), and runs `go vet
-stdmethods=false ./...` (`bytes.Buffer.WriteByte` deliberately returns no
error) and `go test ./...` there.

Two kinds of package:

- **system**: a package that declares builtins: functions, methods and
  types the VM implements, each marked by the last line of its doc
  comment, `//gogo:builtin` (the body, `panic("gogo only")`, is for the
  host toolchain and is never compiled: the compiler turns every call
  into a VM instruction or builtin), and struct types with a fixed type
  index, `//gogo:reserved N` (`TYPE_*` in `runtime/vm/opcodes.h`; the
  compiler checks the index against its table). A system package may also
  hold ordinary library code (functions and methods with real bodies),
  which is compiled into an image only where the program uses it, so
  importing a system package costs nothing by itself. The marks are read
  by `marks.add` in `compiler/load.go`; they are for the `gogo/`
  libraries only.
  A library may hold such functions too:
  `strconv.FormatFloat` and `ParseFloat` (`lib/strconv/float.gg`) are
  compiler intrinsics (`strconvIntrinsics` in `compiler/check.go`), never
  compiled, so the rest of `gogo/strconv` needs no float support.
- **library**: written in the gogo subset and compiled into every image that
  imports it (no dead-function elimination). A library may import system
  packages and other libraries, without cycles, but not a user package (a
  program's own, any import path outside `gogo/`). User packages compile
  like libraries and may import both.

The requirements column lists the names the compiler records in the
image when a package is compiled in (`compiler/modinfo.go`): the driver of
each constant `port.Open` in the package, and `/sd` for a constant `/sd`
path given to `gogo/fs`. An import by itself requires nothing. The
firmware refuses an image that requires a driver or mount it lacks
(`code.Missing` names it). Packages built only on others inherit their
names (for example `gogo/http` via `gogo/dns`).

A file of a library can carry the directive line `//gogo:ondemand`
(conventionally right after the package clause's comment): its functions
(no methods) are compiled into an image only when the program uses them,
directly or through another such function. Everything else in a library
is compiled in whole.

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
| `gogo/actor` | system | | none |
| `gogo/port` | system | actor, bytes | none (each constant `port.Open`: its driver); also the stream protocol every byte port speaks (`Chunk`, `Out`, `Done`, `Closed`) and the host-port types |
| `gogo/bytes` | system | | none |
| `gogo/net` | system | actor, bytes, errors | none (the drivers it is used with: `netif`, `udp`, `tcp`, `tcp_listen`, `tls`) |
| `gogo/gpio` | system | | none (the `gpio` port: `gpio`) |
| `gogo/fs` | system | actor, bytes, errors | none (constant `/sd` paths: `/sd`) |
| `gogo/kbd` | system | | none (the `kbd` port: `kbd`) |
| `gogo/power` | system | | none (the `power` port: `power`) |
| `gogo/code` | system | actor, bytes, errors | none |
| `gogo/imu` | system | | none (the `imu` port: `imu`) |
| `gogo/ir` | system | | none (the `ir_tx` / `ir_rx` ports: `ir_tx` / `ir_rx`) |
| `gogo/audio` | system | | none (the `mic` / `speaker` ports: `mic` / `speaker`) |
| `gogo/sys` | system | | none (the clock, randomness, `sys.Config`, the configuration store, and `sys.Args`, the program's command-line arguments) |
| `gogo/screen` | system | | none (the `screen` port: `screen`) |
| `gogo/errors` | system | | none (`New`, `Is`, `Unwrap`; the shared error codes, `errors.Code`, an `error`; `GOGO_E_*` in `runtime/include/gogo_errors.h`; loaded by every program that uses `error`, compiled only where used) |
| `gogo/math` | system | | `float`, as any use of floats (the functions are `B_FLOAT`, `runtime/floatlib`; a constant used only as an integer, `int(math.MaxInt32)`, requires nothing) |
| `gogo/supervisor` | library | actor, code | none |
| `gogo/strings` | library | | none |
| `gogo/strconv` | library | | none; `float` when `FormatFloat` or `ParseFloat` is called (compiler intrinsics: `B_FLOAT`, their bodies are never compiled). `FormatUint`, `ParseUint` and `ParseInt` (`unsigned.gg`) are compiled in only when called (`//gogo:ondemand`) and written with `int64` operations, so they never need image format 15.3 |
| `gogo/dhcp` | library | actor, bytes, net, port, sys | `netif`, `udp` |
| `gogo/dns` | library | actor, bytes, net, port | `udp` |
| `gogo/http` | library | actor, bytes, dns, errors, fs, net, port, strconv, strings | `tcp`, `tcp_listen`, `udp` (opens the `tls` port for `https://` URLs without requiring it: `//gogo:optional tls`) |
| `gogo/sntp` | library | actor, bytes, dns, net, port, sys | `udp` |
| `gogo/kv` | library | bytes, fs | none |
| `gogo/ringlog` | library | bytes, fs | none |
| `gogo/imulog` | library | actor, bytes, fs, imu, port | `imu` |
| `gogo/wav` | library | bytes, fs | none |
| `gogo/sound` | library | actor, audio, errors, fs, port, wav | none (opens `mic` for Record and `speaker` for Play without requiring them: `//gogo:optional mic speaker`) |
| `gogo/irproto` | library | bytes | none |
| `gogo/tui` | library | actor, bytes, gpio, kbd, port, screen, strconv | `gpio`, `screen` (opens `kbd` without requiring it: `//gogo:optional kbd`) |
