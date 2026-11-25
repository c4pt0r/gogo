# lib: the gogo standard library

Go-subset sources of every `gogo/...` package, one directory per package.
This directory is the Go module `gogo` (`go.mod`), so import path `gogo/X`
is `X/` here for the host Go toolchain too (`gogo/net` is `net/`; the C
network stack is `runtime/net/`). `embed.go` (package `gogo`) embeds the
sources into the compiler with `//go:embed`; the repository root is module
`gogotools`, which requires `gogo` with `replace gogo => ./lib`.

Editors: every package type-checks on the host (stub bodies are
`panic("gogo only")`). A program directory whose `go.mod` has

    require gogo v0.0.0
    replace gogo => <path to>/lib

gets gopls completion and errors for all `gogo/...` imports; `examples/`, the book's examples, `tests/golden/`, `tests/net/progs/`, `tests/footprint/` and `platform/esp32s3/examples/`
are set up this way.

Host checks (`make test-lib`, part of `make test`): `go vet
-stdmethods=false ./...` (`bytes.Buffer.WriteByte` deliberately returns no
error) and `go test ./...` (`irproto/irproto_test.go`).

Two kinds of package:

- **stub**: API only. The bodies are never executed: the compiler replaces
  every call with a VM instruction or builtin, and its struct types have
  fixed type indices (`TYPE_*` in `runtime/vm/opcodes.h`). The list is
  `isStub` in `compiler/load.go`.
- **library**: written in the gogo subset and compiled into every image that
  imports it (no dead-function elimination). A library may import stubs and
  other libraries, without cycles.

The capability column is the `requires` bit the compiler records in the
image (`compiler/modinfo.go`: the import itself, or a constant
`port.Open` driver name inside the package); the firmware refuses an image
whose bits it lacks (`code.Cap*`, `CAP_*` in `runtime/vm/platform.h`).
Packages built only on others inherit their bits (for example `gogo/http`
via `gogo/net`).

| package | kind | imports | capability |
|---------|------|---------|------------|
| `gogo/actor` | stub | | none |
| `gogo/port` | stub | actor, bytes | none (per driver opened) |
| `gogo/bytes` | stub | | none |
| `gogo/net` | stub | actor, bytes | net |
| `gogo/config` | stub | | none |
| `gogo/fs` | stub | actor, bytes | none (`/sd` paths: sd) |
| `gogo/kbd` | stub | | kbd |
| `gogo/power` | stub | | power |
| `gogo/code` | stub | actor, bytes | none |
| `gogo/imu` | stub | | none (the `imu` port: imu) |
| `gogo/ir` | stub | | none (the `ir_tx` / `ir_rx` ports: ir_tx / ir_rx) |
| `gogo/audio` | stub | | none (the `mic` / `spk` ports: mic / spk) |
| `gogo/sys` | stub | | none |
| `gogo/supervisor` | library | actor, code | none |
| `gogo/strings` | library | | none |
| `gogo/strconv` | library | | none |
| `gogo/dhcp` | library | actor, bytes, config, net, port | net |
| `gogo/dns` | library | actor, bytes, net, port | net |
| `gogo/http` | library | actor, bytes, dns, net, port, strconv, strings | net |
| `gogo/sntp` | library | actor, bytes, dns, net, port, sys | net |
| `gogo/kv` | library | bytes, fs | none |
| `gogo/ringlog` | library | bytes, fs | none |
| `gogo/imulog` | library | actor, bytes, fs, imu, port | imu |
| `gogo/wav` | library | bytes, fs | none |
| `gogo/sound` | library | actor, audio, fs, port, wav | mic, spk |
| `gogo/irproto` | library | bytes | none |
