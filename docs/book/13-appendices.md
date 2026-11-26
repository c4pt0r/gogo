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
type GPIO struct{ Pin, Mode, Edge, DebounceMs int }
type Write struct{ Value int }
type Edge struct{ Pin, Value int }
type Host struct{ Arg int; Data bytes.Bytes }   // cfg of a host-defined port (embed mode)
type Post struct{ Kind int; Data bytes.Bytes }  // a message from the embedding host
const (Input = 0; InputPullUp = 1; InputPullDown = 2; Output = 3)
const (None = 0; Rising = 1; Falling = 2; Both = 3)

func Open(driver string, cfg any) (p actor.PID, ok bool)
func Close(p actor.PID)
func Handoff(p actor.PID, to actor.PID) (ok bool)
```

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
type power.Battery struct{ MilliVolts, Percent, Charging, External int }
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
type net.Write struct{ Data bytes.Bytes }              // to "tcp" or "tls"
type net.Shutdown struct{}
type net.Resume struct{}
// events
type net.Up struct{ IP, Gateway, DNS int }
type net.Down struct{}
type net.Datagram struct{ From Addr; Data bytes.Bytes }
type net.Connected struct{}
type net.Data struct{ Data bytes.Bytes }
type net.Written struct{}
type net.Closed struct{ Reason int }
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
	ReadTimeoutMs = 10000; DefaultTimeoutMs = 10000; ListenFailed = 100)
func http.Get(url string, timeoutMs int) (Response, bool)
func http.Post(url, contentType string, body bytes.Bytes, timeoutMs int) (Response, bool)
func http.Do(method, url, contentType string, body bytes.Bytes, maxBody, timeoutMs int) (Response, bool)
func http.HeaderValue(h []Header, name string) (string, bool)
func http.Serve(port int, handler func(Request) Response) actor.PID

// chapter 17: https://, keep-alive, upload
type http.Client struct{ PID actor.PID }
const (http.Failed fs.Err = 100; BadURL fs.Err = 101; TLSMemory = 26624;
	UploadChunk = 4096; LingerMs = 1000; MaxQueued = 4)
func http.NewClient() Client
func http.ClientDo(c Client, method, url, contentType string, body bytes.Bytes, timeoutMs int) (Response, bool)
func http.ClientUpload(c Client, url, path, contentType, token string) (status int, err fs.Err)
func http.CloseClient(c Client)
func http.Upload(url, path, contentType, token string) (status int, err fs.Err)
```

### gogo/config

```go
func config.Get(key string) (value string, ok bool)
```

### gogo/sys and gogo/sntp

```go
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
| 100+ | (yours) | `exit(<code>)` | `actor.Exit(code)` or `actor.Kill(p, code)`; `http.ListenFailed` is 100 |

The crash report is `gogo: actor <pid N> crashed: <kind>[ in <function>]`,
on standard error.

## C. Error codes

**`fs.Err`** (also returned by `kv` and `ringlog`):

| 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `OK` | `NotFound` | `Exists` | `NotMounted` | `NoSpace` | `TooMany` | `Busy` | `BadPath` | `IsDir` | `NotDir` | `NotEmpty` | `NoMemory` | `IO` | `Closed` |

**`net.Closed` reasons:**

| 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|
| `Normal` (peer finished sending) | `Reset` | `Timeout` | `Refused` | `Overflow` (too many Writes) | `NoRoute` (address or link lost) | `NoTime` (the wall clock is not set) | `BadCert` (the certificate failed verification) | `TLS` (any other TLS failure) |

**`code.Err`** (the result of every `gogo/code` call):

| 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|
| `OK` | `BadImage` | `Unsupported` (firmware lacks a driver or mount: see `Missing`) | `NoMemory` (over `code.mem`) | `NotFound` | `BadArg` | `TooMany` (module table or holders full) | `BadVersion` (another image format) |

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
| floats, complex, `uint`, `int8/16/32`, `uint16/32/64`, `uintptr` | none; `int`, `int64`, `byte` |
| arrays, maps, pointers | slices only |
| methods, interfaces | functions; type switches over `any` |
| `p.X = v` | build a new struct |
| `switch` on an interface or a struct value, `goto` | type switch, compare fields; labeled `break`/`continue` |
| type assertions `x.(T)` | type switch |
| generics | none |
| `error`, `panic`, `recover`, `defer` | `bool`/code results; `actor.Exit`; supervisors; automatic cleanup |
| package-level `var`, `init` | `const`; state in actors |
| multiple packages, the standard library | one `package main`; `gogo/...` packages only |
| `fmt.Println` | `println` (prints slices' elements; Go's `println` prints an address) |
| UTF-8 aware `range` over strings | bytes |
| `go run` | `gogo run`; `gogo build` + `gogo flash` |
| runtime panics crash the program | runtime errors end only the actor |

## F. Requirement names

The compiler records in every image and module, by name, the drivers and
mounts it needs from the firmware (its requirements); a firmware that
lacks one refuses the image at boot, naming it, and `code.Load` returns
`Unsupported` with `code.Missing` naming it (chapter 14). `gogo firmware`
(chapter 12) picks the firmware switches from the names.

| Name | Required by | `gogo firmware` feature | Firmware switch |
|---|---|---|---|
| `gpio` | the `gpio` port | (always built) | |
| `/flash` | (never inferred: every firmware has it) | (always built) | |
| `netif`, `udp`, `tcp`, `tcp_listen` | the network ports, opened by `gogo/dhcp`, `dns`, `http`, `sntp` or directly with `gogo/net` | `net` | `CONFIG_GOGO_NET` |
| `kbd` | the `kbd` port (`gogo/kbd`) | `kbd` | `CONFIG_GOGO_KBD` |
| `power` | the `power` port (`gogo/power`) | `power` | `CONFIG_GOGO_BATTERY` |
| `/sd` | a constant `/sd` path in a `gogo/fs` call | `sd` | `CONFIG_GOGO_SD` |
| `imu` | the `imu` port (`gogo/imu`, `gogo/imulog`) | `imu` | `CONFIG_GOGO_IMU` |
| `ir_tx` | the `ir_tx` port | `irtx` | `CONFIG_GOGO_IR` |
| `ir_rx` | the `ir_rx` port | `irrx` | `CONFIG_GOGO_IR` |
| `mic` | the `mic` port opened directly; `gogo/sound` opens it for `Record` but does not require it (`//gogo:optional`, chapter 12) | `mic` | `CONFIG_GOGO_AUDIO` |
| `spk` | the `spk` port opened directly; `gogo/sound` opens it for `Play` but does not require it | `spk` | `CONFIG_GOGO_AUDIO` |
| `tls` | the `tls` port opened directly (`net.DialTLS`); `gogo/http` opens it for `https://` URLs but does not require it (`//gogo:optional`, chapters 12 and 17) | `tls` | `CONFIG_GOGO_TLS` (needs `CONFIG_GOGO_NET`) |
| a host port's name | `port.Open` of a port the embedding program defines (chapter 18) | (none: an embedded VM only) | |

An import alone requires nothing: `gogo/sys` (the wall clock and `Random`
work on every firmware), `gogo/fs` on `/flash`, and the type-only use of
`gogo/net` or `gogo/kbd` need no name. A port opened by a name known only
at run time requires nothing (with a compiler warning); the open just
fails on a firmware without the driver.
