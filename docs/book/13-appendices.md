# 13. Appendices

## A. Library quick reference

The authoritative definitions are the package sources in the gogo tree,
one directory per package under `lib/` (`lib/actor/`, `lib/port/`,
`lib/bytes/`, `lib/net/` for `gogo/net`, and so on; `lib/README.md` lists
them all); their comments give every detail.

### gogo/actor

```go
type PID struct{ /* opaque; comparable */ }
type Ref struct{ /* opaque; comparable */ }
type Reason struct{ Code int; Func string }
type Options struct{ MaxMemory int }          // bytes; 0 = platform default
type ProcInfo struct{ Memory int }
type ExitMsg struct{ From PID; Reason Reason }
type DownMsg struct{ Ref Ref; From PID; Reason Reason }
type Request struct{ From PID; Ref Ref; Body any }

func Spawn(f func()) PID
func SpawnLink(f func()) PID
func SpawnOpt(f func(), opts Options) PID
func Self() PID
func Send(to PID, msg any)
func Receive() any
func ReceiveTimeout(ms int) (msg any, ok bool)
func Call(to PID, req any, timeoutMs int) (reply any, ok bool)
func Reply(req Request, reply any)
func Link(p PID)
func Unlink(p PID)
func Monitor(p PID) Ref
func Demonitor(r Ref)
func TrapExit(on bool)
func Exit(code int)
func Kill(p PID, code int)
func MakeRef() Ref
func Register(name string, p PID) bool
func Whereis(name string) (PID, bool)
func Unregister(name string)
func SendAfter(ms int, to PID, msg any) Ref
func CancelTimer(r Ref) bool
func Sleep(ms int)
func Now() int64
func Info(p PID) ProcInfo
```

### gogo/supervisor

```go
const (Permanent = 0; Transient = 1)
type Spec struct{ Restart, MaxRestarts, PeriodMs int }
func Supervise(child func(), spec Spec) actor.PID
func SuperviseModule(m code.Module, entry string, arg any, spec Spec) actor.PID
type Upgrade struct{ Module code.Module }     // actor.Call(sup, Upgrade{m}, ms): 0 is ok
```

### gogo/code

```go
type Module struct{ H int; missing string }   // H == 0: none
type Err int                                  // OK, BadImage, Unsupported, NoMemory, NotFound, BadArg, TooMany, BadVersion

func Load(b bytes.Bytes) (Module, Err)
func Spawn(m Module, entry string, arg any) (actor.PID, Err)
func SpawnLink(m Module, entry string, arg any) (actor.PID, Err)
func SpawnOpt(m Module, entry string, arg any, opts actor.Options) (actor.PID, Err)
func Hold(m Module) Err
func Release(m Module)
func Missing(m Module) string                 // after Unsupported: the missing driver or mount
func MemUsed() int
```

Chapter 14 describes them.

### gogo/port

```go
// the stream protocol of every byte port (tcp, tls, imu, ir, mic, speaker, screen)
type Chunk struct{ From actor.PID; Seq, TimeMs, Dropped int; Data bytes.Bytes }  // event: input
type Out struct{ Data bytes.Bytes }                 // command: send Data (2 in flight)
type Done struct{ From actor.PID; Seq int }         // event: an Out finished (Seq -1: not sent)
type Closed struct{ From actor.PID; Reason int }    // event: the port ended by itself
// embed mode
type Host struct{ Arg int; Data bytes.Bytes }                  // cfg of a host-defined port
type Post struct{ From actor.PID; Kind int; Data bytes.Bytes }  // a message from the embedding host

func Open(driver string, cfg any) (p actor.PID, ok bool)
func Close(p actor.PID)
func Handoff(p actor.PID, to actor.PID) (ok bool)
```

### gogo/gpio

```go
type Config struct{ Pin, Mode, Edge, DebounceMs int }  // cfg of "gpio"
type Write struct{ Value int }                         // command
type Edge struct{ Pin, Value int }                     // event
const (Input = 0; InputPullUp = 1; InputPullDown = 2; Output = 3)
const (None = 0; Rising = 1; Falling = 2; Both = 3)
```

### gogo/errors

```go
type Code int   // OK (0), a shared code (1..35), or a package's own (>= 100)
const (OK = 0; NotFound = 1; Exists = 2; NoMemory = 3; TooMany = 4; Busy = 5; BadArg = 6;
	Closed = 7; IO = 8; Timeout = 9; Unsupported = 10; NoSpace = 11; BadPath = 12; IsDir = 13;
	NotDir = 14; NotEmpty = 15; NotMounted = 16; BadImage = 17; Refused = 18; Reset = 19;
	NoRoute = 20; BadCert = 21; NoTime = 22; TLS = 23; Overflow = 24; BadURL = 25;
	BadFormat = 26; BadVersion = 27)
```

Appendix C lists who uses which. `gogo/http`'s `Upload` and
`ClientUpload` and `gogo/sound`'s `Recorded.Err` and `Played.Err` are
`errors.Code`.

### gogo/bytes

```go
func NewBuffer(capacity int) Buffer
func FromString(s string) Bytes
func FromSlice(p []byte) Bytes

func (b Buffer) WriteString(s string)
func (b Buffer) Write(p []byte)
func (b Buffer) WriteByte(c byte)
func (b Buffer) WriteBytes(x Bytes)
func (b Buffer) Len() int
func (b Buffer) Freeze() Bytes

func (x Bytes) Len() int
func (x Bytes) At(i int) byte
func (x Bytes) Slice(lo, hi int) Bytes
func (x Bytes) String() string
func (x Bytes) Equal(y Bytes) bool
func (x Bytes) CopyTo(p []byte) int
```

(These are the only methods in the subset: the compiler turns them into
VM builtins.)

### gogo/strings and gogo/strconv

```go
func strings.Index(s, sub string) int
func strings.IndexByte(s string, c byte) int
func strings.Cut(s, sep string) (before, after string, found bool)
func strings.HasPrefix(s, prefix string) bool
func strings.HasSuffix(s, suffix string) bool
func strings.TrimSpace(s string) string          // ASCII white space
func strings.ToLower(s string) string            // ASCII letters
func strings.EqualFold(s, t string) bool         // ASCII letters
func strings.Split(s, sep string) []string       // sep "" splits into bytes

func strconv.Itoa(i int) string
func strconv.Atoi(s string) (n int, ok bool)
func strconv.FormatInt64(i int64, base int) string        // "" for a bad base
func strconv.ParseInt64(s string, base int) (n int64, ok bool) // no base 0, no _
func strconv.FormatUint(i uint64, base int) string          // "" for a bad base
func strconv.ParseUint(s string, base, bitSize int) (n uint64, ok bool) // bitSize 0: 32
func strconv.ParseInt(s string, base, bitSize int) (n int64, ok bool)   // bitSize 0: 32
```

### gogo/kbd and gogo/power

```go
type kbd.Config struct{ RepeatDelayMs, RepeatMs int }
type kbd.Key struct{ Code, Char, Mods int; Down bool }
const (kbd.ModCtrl = 1; ModShift = 2; ModAlt = 4; ModOpt = 8; ModFn = 16)
// key codes: Enter 0x28, Esc 0x29, Backspace 0x2A, Tab 0x2B, Space 0x2C,
// F1..F12 0x3A..0x45, Delete 0x4C, Right 0x4F, Left 0x50, Down 0x51,
// Up 0x52, LeftCtrl 0xE0, LeftShift 0xE1, LeftAlt 0xE2, Opt 0xE3, Fn 0xF0

type power.Config struct{ PeriodMs int }
type power.Battery struct{ MilliVolts, Pct, Charging, External int }
type power.Read struct{}
```

### gogo/fs, gogo/kv, gogo/ringlog

```go
type fs.File actor.PID
type fs.Err int
type fs.Info struct{ Size int; IsDir bool }
type fs.Space struct{ Used, Total int }
const (fs.Read = 0; fs.Write = 1; fs.Append = 2)

func fs.Open(path string, mode int) (File, Err)
func (f File) Read(max int) (bytes.Bytes, Err)
func (f File) Write(data any) Err   // string, bytes.Bytes or []byte
func (f File) WriteAt(off int, data any) Err // Write-mode files, inside the file only
func (f File) Sync() Err
func (f File) Size() (int, Err)
func (f File) Close()
func fs.ReadFile(path string, max int) (bytes.Bytes, Err)
func fs.WriteFile(path string, data any) Err
func fs.Stat(path string) (Info, Err)
func fs.ReadDir(path string) ([]string, Err)
func fs.Mkdir(path string) Err
func fs.Remove(path string) Err
func fs.Rename(from, to string) Err
func fs.Usage(prefix string) (Space, Err)

const kv.MaxValue = 4096
func kv.Get(key string) (bytes.Bytes, fs.Err)
func kv.Put(key string, v any) fs.Err  // string or bytes.Bytes
func kv.Delete(key string) fs.Err
func kv.Keys() ([]string, fs.Err)

func ringlog.Append(dir string, maxBytes int, line any) fs.Err
func ringlog.Read(dir string, max int) (bytes.Bytes, fs.Err)
```

### gogo/net, gogo/dhcp, gogo/dns, gogo/http

```go
// port configs
type net.IfConfig struct{}
type net.UDP struct{ Port int }
type net.Dial struct{ IP, Port, TimeoutMs int }
type net.DialTLS struct{ IP, Port, TimeoutMs int; Host string }  // "tls"
type net.Listen struct{ Port int }
// commands
type net.SetAddr struct{ IP, Mask, Gateway, DNS int }  // to "netif", owner only
type net.SendTo struct{ Addr Addr; Data bytes.Bytes }  // to "udp"
// to "tcp" or "tls": port.Out{Data}, answered by port.Done
type net.Shutdown struct{}
type net.Resume struct{}
// events
type net.Up struct{ IP, Gateway, DNS int }
type net.Down struct{}
type net.Datagram struct{ From Addr; Data bytes.Bytes }
type net.Connected struct{}
// from "tcp" or "tls": port.Chunk, port.Done, port.Closed{From, Reason}
type net.Accepted struct{ Conn actor.PID }
type net.Addr struct{ IP, Port int }
func net.Subscribe() (ok bool)

type dhcp.Config struct{ TimeoutMs int }
func dhcp.Start(cfg Config) bool

func dns.Lookup(name string, timeoutMs int) (int, bool)

type http.Header struct{ Name, Value string }
type http.Request struct{ Method, Path string; Headers []Header; Body bytes.Bytes }
type http.Response struct{ Status int; Headers []Header; Body bytes.Bytes; Truncated bool }
const (http.MaxBody = 8192; MaxBodyLimit = 65536; MaxHead = 2048;
	ReadTimeoutMs = 10000; DefaultTimeoutMs = 10000; ListenFailed = 69)   // actor.Failed + errors.Busy
func http.Get(url string, timeoutMs int) (Response, bool)
func http.Post(url, contentType string, body bytes.Bytes, timeoutMs int) (Response, bool)
func http.Do(method, url, contentType string, body bytes.Bytes, maxBody, timeoutMs int) (Response, bool)
func http.HeaderValue(h []Header, name string) (string, bool)
func http.Serve(port int, handler func(Request) Response) actor.PID

// chapter 17: https://, keep-alive, upload
type http.Client struct{ PID actor.PID }
const (http.BadURL = errors.BadURL; TLSMemory = 26624;
	UploadChunk = 4096; LingerMs = 1000; MaxQueued = 4)
func http.NewClient() Client
func http.ClientDo(c Client, method, url, contentType string, body bytes.Bytes, timeoutMs int) (Response, bool)
func http.ClientUpload(c Client, url, path, contentType, token string) (status int, err errors.Code)
func http.CloseClient(c Client)
func http.Upload(url, path, contentType, token string) (status int, err errors.Code)
```

### gogo/sys and gogo/sntp

```go
func sys.Config(key string) (value string, ok bool)   // the configuration store
func sys.Args() []string        // the program's arguments, without its name; empty on a board
func sys.Time() int64           // Unix time in ms (UTC); 0: unknown
func sys.SetTime(ms int64) bool // false: earlier than the build, refused; ms <= 0 forgets
func sys.Random() int           // 30 random bits

func sntp.Sync(server string, timeoutMs int) bool // sets sys's clock; timeoutMs <= 0: 5000
func sntp.Now() int64                             // sys.Time()
```

Chapter 10.8 describes them. The packages for the motion sensor, infrared
and audio (`gogo/imu`, `gogo/ir`, `gogo/audio` and the libraries built on
them) are described in chapters 15 and 16.

## B. Exit reasons

| Code | Constant | Crash report | Cause |
|---|---|---|---|
| 0 | `actor.Normal` | (none) | the entry function returned |
| 1 | `actor.Killed` | `killed` | `actor.Kill(p, actor.Killed)`; cannot be trapped |
| 2 | `actor.OutOfMemory` | `oom` | over the actor's limit (also deep recursion), or the VM is out of memory |
| 3 | `actor.MailboxOverflow` | `mailbox_overflow` | queued messages took the receiver over its limit |
| 4 | `actor.NoProc` | `noproc` | linked to an actor that does not exist |
| 5 | `actor.DivZero` | `divzero` | `/` or `%` by zero |
| 6 | `actor.BadCode` | `badcode` | index or slice out of range, nil function value called, unusable zero `Buffer`, a bad image |
| 7 | `actor.Shutdown` | (none) | asked to stop; supervisors use it |
| 8 | `actor.BadShift` | `badshift` | negative shift count |
| 9 | `actor.BadMsg` | `badmsg` | sent a message holding a module's own struct or closure to an actor of another module (the sender exits; chapter 14) |
| 10 | `actor.Panic` | `panic` | `panic(v)` (which first prints `panic: v` on standard error) |
| 11..63 | | | kept for the VM |
| 64..99 | `actor.Failed` + a shared code | `exit(<code>)` | a library's failure: `actor.Failed` (64) plus a shared code, 1..35 (appendix C); `http.ListenFailed` is 69, `actor.Failed + errors.Busy` |
| 100+ | (yours) | `exit(<code>)` | `actor.Exit(code)` or `actor.Kill(p, code)` |

The crash report is `gogo: actor <pid N> crashed: <kind>[ in <function>]`,
on standard error. The reason `main` exits with is also the exit status of
`gogo run` and `gogovm`: 0 for `Normal` and `Shutdown`, 4 for a VM reason
(1..63) or a negative code, the code itself for 64..255, and 255 above
that (chapter 12).

## C. Error codes

**The shared codes** (`gogo/errors`, `GOGO_E_*` in
`runtime/include/gogo_errors.h` on the C side): one numbering for every
meaning the packages share. An `errors.Code` is an `error`; its `Error()`
is the text `gogo_strerror` gives the same code on the C side (`"not
found"`, `"busy"`, ...), and `"error N"` for a code without one.

| Range | Codes |
|---|---|
| 0 | `OK` |
| 1..27 | `NotFound`=1 `Exists`=2 `NoMemory`=3 `TooMany`=4 `Busy`=5 `BadArg`=6 `Closed`=7 `IO`=8 `Timeout`=9 `Unsupported`=10 `NoSpace`=11 `BadPath`=12 `IsDir`=13 `NotDir`=14 `NotEmpty`=15 `NotMounted`=16 `BadImage`=17 `Refused`=18 `Reset`=19 `NoRoute`=20 `BadCert`=21 `NoTime`=22 `TLS`=23 `Overflow`=24 `BadURL`=25 `BadFormat`=26 `BadVersion`=27 |
| 28..35 | free for later shared meanings; shared codes stop at 35 so that `actor.Failed` + a shared code stays below 100 (appendix B) |
| 36..99 | reserved, never shared codes |
| 100 and up | one block of 20 per package that needs its own codes: `gogo/http` 100..119, `gogo/sound` 120..139 (neither uses its block yet) |

**`fs.Err`** (also returned by `kv`, `ringlog`, `imulog` and `wav`): the
shared codes, as `fs.Err(errors.X)` under the same names. `kv.Put` also
returns `fs.Err(errors.Overflow)` (24) for a value over 4096 bytes and
`fs.Err(errors.BadArg)` (6) for one that is neither a string nor `Bytes`;
`imulog.Append` returns `fs.Err(errors.Overflow)` for a payload over
65535 bytes.

| 0 | 1 | 2 | 3 | 4 | 5 | 7 | 8 | 11 | 12 | 13 | 14 | 15 | 16 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `OK` | `NotFound` | `Exists` | `NoMemory` | `TooMany` | `Busy` | `Closed` | `IO` | `NoSpace` | `BadPath` | `IsDir` | `NotDir` | `NotEmpty` | `NotMounted` |

**`port.Closed` reasons of `"tcp"` and `"tls"`** (the `net` constants, plain
`int`s): the shared codes, with `Normal` = `OK`.

| 0 | 9 | 18 | 19 | 20 | 21 | 22 | 23 |
|---|---|---|---|---|---|---|---|
| `Normal` (peer finished sending) | `Timeout` | `Refused` | `Reset` | `NoRoute` (address or link lost) | `BadCert` (the certificate failed verification) | `NoTime` (the wall clock is not set) | `TLS` (any other TLS failure) |

**`code.Err`** (the result of every `gogo/code` call): the shared codes,
as `code.Err(errors.X)` under the same names.

| 0 | 1 | 3 | 4 | 6 | 10 | 17 | 27 |
|---|---|---|---|---|---|---|---|
| `OK` | `NotFound` | `NoMemory` (over `code.mem`) | `TooMany` (module table or holders full) | `BadArg` | `Unsupported` (firmware lacks a driver or mount: see `Missing`) | `BadImage` | `BadVersion` (another image format) |

## D. Limits

| Limit | Board (ESP32-S3) | Host |
|---|---|---|
| memory for all actors | 160 KB | the computer's |
| default memory limit per actor | 16 KB | 256 KB |
| largest memory limit per actor | 256 KB | 256 KB |
| actors at once | 1024 | 16384 |
| idle actor | about 150 bytes | about 150 bytes |
| reductions per time slice | 1000 | 1000 |
| open ports | 16 | 16 |
| open files | 4 | 4 |
| off-heap `Bytes` per actor / in the VM | 16 / 64 | 16 / 64 |
| `Bytes` shared instead of copied | over 64 bytes, from `Freeze` | same |
| TCP connections | 4 | (no network) |
| `net.Subscribe` subscribers | 4 | (no network) |
| HTTP head / body | 2 KB / 8 KB (client body up to 64 KB with `Do`) | (no network) |
| TLS connections | 1 (about 23 KB charged to its owner) | (no network) |
| config key / value | 15 chars / 1000 bytes | same |
| file name / path | 32 / 96 bytes | same |
| `kv` value | 4096 bytes | same |
| program image | 1 MB | |
| modules loaded at once (the main program included) | 8 | 8 |
| holders per module (`code.Hold`, the loader) | 4 | 4 |
| module code in memory (`code.mem`) | 64 KB (config key `code.mem`, `K`/`M` suffix) | 64 KB |

## E. Differences from Go at a glance

| Go | gogo |
|---|---|
| goroutines, channels, `select` | actors: `actor.Spawn`, `Send`, `Receive` |
| shared memory between goroutines | none; every actor has its own heap |
| sending a slice shares it | `Send` deep-copies (large frozen `Bytes` are shared, read-only) |
| closures capture variables by reference | closures copy captured values; captured variables must not be reassigned |
| `int` is 64 bits on most machines | `int` is always 32 bits |
| `uint` is 64 bits on most machines | `uint` is 32 bits, like `int`; `int8` .. `uint64` as in Go, but not in an `any` |
| complex, `uintptr` | none |
| maps, pointers | slices of structs, searched |
| methods with pointer receivers | value receivers that return the new value |
| `goto` | labeled `break`/`continue` |
| generics | none |
| `error`, `panic`, `recover` | `bool`/code results; `actor.Exit`; supervisors; automatic cleanup |
| `defer` anywhere, run on panics too | `defer` outside loops only, run on normal returns only |
| package-level `var`, `init` | `const`; state in actors |
| multiple packages, the standard library | one `package main`; `gogo/...` packages only |
| `fmt.Println` | `println` (prints slices' elements; Go's `println` prints an address) |
| UTF-8 aware `range` over strings | bytes |
| `go run` | `gogo run`; `gogo build` + `gogo flash` |
| runtime panics crash the program | runtime errors end only the actor |

## F. Requirement names

The compiler records in every image and module, by name, the drivers and
mounts it needs from the firmware, and `float` when it uses floating
point (its requirements); a firmware that
lacks one refuses the image at boot, naming it, and `code.Load` returns
`Unsupported` with `code.Missing` naming it (chapter 14). `gogo firmware`
(chapter 12) picks the firmware switches from the names.

| Name | Required by | `gogo firmware` feature | Firmware switch |
|---|---|---|---|
| `gpio` | the `gpio` port | (always built) | |
| `/flash` | (never inferred: every firmware has it) | (always built) | |
| `netif`, `udp`, `tcp`, `tcp_listen` | the network ports, opened by `gogo/dhcp`, `dns`, `http`, `sntp` or directly with `gogo/net` | `net` | `CONFIG_GOGO_NET` |
| `kbd` | the `kbd` port (`gogo/kbd`); `gogo/tui` opens it but does not require it | `kbd` | `CONFIG_GOGO_KBD` |
| `power` | the `power` port (`gogo/power`) | `power` | `CONFIG_GOGO_BATTERY` |
| `/sd` | a constant `/sd` path in a `gogo/fs` call | `sd` | `CONFIG_GOGO_SD` |
| `imu` | the `imu` port (`gogo/imu`, `gogo/imulog`) | `imu` | `CONFIG_GOGO_IMU` |
| `ir_tx` | the `ir_tx` port | `irtx` | `CONFIG_GOGO_IR` |
| `ir_rx` | the `ir_rx` port | `irrx` | `CONFIG_GOGO_IR` |
| `mic` | the `mic` port opened directly; `gogo/sound` opens it for `Record` but does not require it (`//gogo:optional`, chapter 12) | `mic` | `CONFIG_GOGO_AUDIO` |
| `speaker` | the `speaker` port opened directly; `gogo/sound` opens it for `Play` but does not require it | `speaker` | `CONFIG_GOGO_AUDIO` |
| `tls` | the `tls` port opened directly (`net.DialTLS`); `gogo/http` opens it for `https://` URLs but does not require it (`//gogo:optional`, chapters 12 and 17) | `tls` | `CONFIG_GOGO_TLS` (needs `CONFIG_GOGO_NET`) |
| `screen` | the `screen` port (`gogo/screen`, `gogo/tui`; chapter 19) | `screen` | `CONFIG_GOGO_SCREEN` |
| `float` | floating point anywhere in the image: a float value, operation or constant, a `float32` / `float64` type switch case, `gogo/math`, `strconv.FormatFloat` / `ParseFloat` (not the rest of `gogo/strconv`) | `float` | `CONFIG_GOGO_FLOAT` |
| a host port's name | `port.Open` of a port the embedding program defines (chapter 18) | (none: an embedded VM only) | |

An import alone requires nothing: `gogo/sys` (the wall clock and `Random`
work on every firmware), `gogo/fs` on `/flash`, and the type-only use of
`gogo/net` or `gogo/kbd` need no name. A port opened by a name known only
at run time requires nothing (with a compiler warning); the open just
fails on a firmware without the driver.

## G. Bytecode reference

For the instruction set itself (every opcode with its number, operands and
effect, the builtins table, the reserved types and the image file layout) see
the generated [bytecode reference](../bytecode.md). `gogo dis file.ggb`
prints the bytecode of a program, and the design behind it is in
[design.md](../design.md) section 5.6.
