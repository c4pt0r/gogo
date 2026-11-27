# Known issues and limitations

Open limitations of gogo at image format 15 (the current minor is in
`runtime/vm/image.h`; `gogo version` prints it), by area. Each entry
says what the limitation is, what it costs you and, where there is one,
how to work around it. Hardware facts refer to the two ESP32-S3 boards,
the M5StickS3 and the Cardputer ADV. The design is in
[design.md](design.md); the book's appendix lists the documented limits
([13-appendices.md](book/13-appendices.md)).

## Breaking changes since v0.1.0

Programs written for v0.1.0 need these edits, and every image must be
rebuilt:

- **`actor.Call`** returns `(reply any, err error)` instead of `(reply
  any, ok bool)`: `err` is `nil`, `errors.Timeout`, `errors.NoProc` (no
  live local actor) or `errors.NoConnection` (a remote PID's node is
  gone), book 4.3. `r, ok := actor.Call(...); if !ok {` becomes `r, err
  := actor.Call(...); if err != nil {`; a program that only checked `ok`
  needs no other change, and the compiler rejects an `ok` still used as
  a `bool` (`!ok`, `if ok`). A program that calls `actor.Call` or
  `actor.Register`, itself or through a library call that does (the
  libraries keep those functions in `//gogo:ondemand` files, so only
  the calls that use one: every `gogo/node` call, `dns.Lookup`,
  `dhcp.Start`, `sntp.Sync`, `http`'s client calls (`Get`, `Post`, `Do`,
  `NewClient`, `ClientDo`, `Upload`, ...), `supervisor.SuperviseModule`,
  `sound.Record` and `tui.Open` count), is image format 15.7 and
  needs firmware that runs 15.7; a program that uses only the rest
  (`supervisor.Supervise`, `http.Serve`, `sound.Play`, `tui.Width`, ...)
  keeps its minor; images built before
  keep running on the new firmware. `gogo/errors` gained the shared codes
  `NoProc` (28) and `NoConnection` (29); `node.Whereis` and the other
  `gogo/node` calls now return the Call's error (`NoConnection` when the
  node goes during the call) where they returned `errors.Timeout`.
- **`actor.Register`** returns an `error` instead of a `bool`: `nil`,
  `errors.NoProc` (the PID is not a live local actor) or `errors.Exists`
  (the name is taken). `if actor.Register(n, p) {` becomes `if
  actor.Register(n, p) == nil {`, and `if !actor.Register(...)` becomes
  `!= nil`; a call used as a statement needs no edit (it compiles to the
  builtin alone, so it costs a few bytes, not the error's ~500). Like
  `Call`, it makes the image 15.7. `actor.Whereis` is unchanged.
- **`actor.SendAfter`** takes its arguments in `Send`'s order with the
  delay last, as `Call` does: `actor.SendAfter(ms, p, msg)` is
  `actor.SendAfter(p, msg, ms)`. Every old call fails to compile (an
  `int` where the PID goes), so none is missed. The VM builtin is
  unchanged: old images run as before, and a rewritten call compiles to
  the same code.

- **Images.** The reserved type numbers changed inside format 15.0, so an
  image built by an older compiler, including one built between the
  image format 15 change and this one, loads but misreads types. Rebuild
  every `.ggb`, hot-loadable modules on `/flash` included.
- **tcp and tls** speak the stream protocol: `net.Write{Data}` is
  `port.Out{Data}`, `net.Written` is `port.Done` (a third Out in flight
  gets `Done{Seq: -1}` instead of closing the connection), `net.Data` is
  `port.Chunk` and `net.Closed` is `port.Closed`; `net.Overflow` is gone.
- **Event sources.** `port.Chunk`, `port.Done` and `port.Post` gained
  `From` as their first field: positional literals need it.
- **GPIO** types moved to `gogo/gpio`: `port.GPIO` is `gpio.Config`,
  `port.Write` and `port.Edge` are `gpio.Write` and `gpio.Edge`, and the
  mode and edge constants are `gpio.Input` ... `gpio.Both`.
- **Configuration.** `gogo/config` is gone: `config.Get(key)` is
  `sys.Config(key)`.
- **Units.** `audio.Mic{Rate, FrameMs, Gain}` is `{RateHz, FrameMs,
  GainDb}`, `audio.Speaker{Rate, Volume}` is `{RateHz, VolumePct}`,
  `audio.MaxGain` and `MaxVolume` are `MaxGainDb` and `MaxVolumePct`, and
  `power.Battery.Percent` is `Pct`.
- **Speaker.** The driver `"spk"` is `"speaker"`, in `port.Open`, in
  `//gogo:optional` and as a `gogo firmware` feature (`-without spk` is
  now `-without speaker`).
- **Error codes** are one space, `gogo/errors` (book appendix C), and
  the numbers changed inside format 15. `fs.Err` and `code.Err` keep
  their names, but besides `OK` only `fs.NotFound`, `fs.Exists` and
  `code.NoMemory` keep their values (`fs.NotMounted` was 3 and is 16,
  `code.NotFound` was 4 and is 1, ...). The `port.Closed` reasons of tcp
  and tls (`net.Reset` ... `net.TLS`) are the shared codes too
  (`net.Timeout` was 2 and is 9, ...; `Normal` stays 0). Source that uses
  the names needs no edit; source or tools that compare with numbers do.
  `gogo/http` and `gogo/sound` no longer borrow `fs.Err`: `Upload`,
  `ClientUpload`, `Recorded.Err` and `Played.Err` are `errors.Code`, so
  compare them with `errors.NoSpace` and the like, not `fs.NoSpace` (now
  a type mismatch): `if m.Err == fs.NoSpace` becomes `if m.Err ==
  errors.NoSpace`, with `import "gogo/errors"`. `http.Failed` (100) is the connection's
  `Closed` reason, `errors.Timeout` or `errors.IO`, `http.BadURL` is `errors.BadURL` (25),
  `sound.SpkFailed` is `errors.IO`, `kv.Put` of a value over 4096 bytes
  is `errors.Overflow` (was `fs.NoMemory`) and of another type
  `errors.BadArg` (was `fs.BadPath`), `imulog.Append` of a payload over
  65535 bytes `errors.Overflow` (was `fs.NoMemory`), all as `fs.Err`, and `sound.BadWav`, which nothing
  returned, is gone (`errors.BadFormat` is the shared code for it).
  `http.ListenFailed` is 69 instead of 100: exit codes 64..99 are library
  failures now (`actor.Failed` plus a shared code); user codes still
  start at 100. An image built before this change still runs on new
  firmware, but the codes it compiled into its own comparisons are the
  old ones while the firmware returns the new ones: rebuild it.
- **`fs.Err` and `code.Err` are `errors.Code`** (type aliases, with
  interfaces and `error` in format 15.4): every package's error codes are
  one type, so `err == errors.NotFound` holds for an error from any of
  them, and `fs.NotFound`, `code.BadImage` ... are the same constants as
  `errors.NotFound`, `errors.BadImage`. Conversions between them still
  compile (they do nothing now); a type switch or a switch that names
  both `fs.Err` and `errors.Code` (or `code.Err`) as separate cases no
  longer compiles (duplicate case): keep one.
- **Go signatures.** A library function named like a Go function has
  the Go signature, and one that can fail for several reasons returns an
  `error` (format 15.4):
  - `port.Open(driver, cfg)` returns `(actor.PID, error)`, the error an
    `errors.Code` (`NotFound` for a driver the board lacks, `BadArg`,
    `Busy`, `TooMany`, `NoMemory`): `p, ok := port.Open(...)` / `if !ok`
    becomes `p, err := port.Open(...)` / `if err != nil`. An image that
    opens a port needs firmware of format 15.4 or later.
  - `strconv.Atoi(s)`, `ParseInt`, `ParseUint` and `ParseFloat` return
    an `error` in place of `ok bool`: `errors.BadFormat`,
    `errors.Overflow` (with the value clamped, as in Go, where it was 0)
    or, for a bad base or bit size, `errors.BadArg`. `n, ok :=` / `if
    !ok` becomes `n, err :=` / `if err != nil`.
  - `http.Get`, `Post`, `Do` and `ClientDo` return `(Response, error)`
    (`errors.BadURL`, `Timeout` or `IO`), `dns.Lookup` returns `(int,
    error)` (`NotFound`, `NoRoute`, `Timeout`, `BadArg`), `sntp.Sync` and
    `dhcp.Start` return an `error` instead of a `bool`: `if
    !dhcp.Start(cfg)` becomes `if dhcp.Start(cfg) != nil`.
- **Embedding API** (`gogo.h`; hosts must be edited and rebuilt, see
  the book's chapter 18):
  - `gogo_host` and `gogo_config` begin with a `size`: start them from
    `GOGO_HOST_INIT` and `GOGO_CONFIG_INIT` (a table without its size is
    refused). Fill tables with designated initializers; positional ones
    no longer line up. `gogo_new` copies the host table.
  - `gogo_new(h, ctx, cfg)` and `gogo_new_posix(cfg)` return a result
    code and give the VM through a last argument, `&vm`.
  - `gogo_host.random_bytes` is gone (nothing called it). The new
    `destroy` hook frees the host's context after the VM's last
    reference.
  - `set_unix_time_ms` returns 0 to accept and a negative `GOGO_E*` to
    refuse (it returned 1 and 0).
  - `gogo_config.allow_code_load` is the flag `GOGO_CONFIG_CODE_LOAD` in
    `flags`; `mem_limit` is a `uint64_t` and `max_actors` a `uint32_t`.
  - The POSIX host and `gogo_run_thread` / `gogo_join` moved to
    `gogo_posix.h` (which includes `gogo.h`): a host that uses them
    includes that header instead.
  - Other threads post through a `gogo_poster` (`gogo_poster_new` on
    the owner's thread, then `gogo_poster_post`, `gogo_poster_port_post`,
    `gogo_poster_close_posts`, `gogo_poster_free`): `gogo_retain`,
    `gogo_release` and `gogo_port_post` are gone, and `gogo_post` and
    `gogo_close_posts` are for the VM's own thread.
  - Names take a length: `gogo_post(vm, name, name_len, kind, data,
    len)` and `gogo_register_port(vm, name, name_len, drv, ctx)`.
  - `gogo_port` is a `uint32_t` (it was a struct of index and
    generation); compare it with `==`. `gogo_port_driver` and
    `gogo_stats` begin with a `size` too (`GOGO_PORT_DRIVER_INIT`,
    `GOGO_STATS_INIT`); a stats counter is a `uint64_t`, and
    `gogo_get_stats` returns a result code. A driver's `open` refuses
    with a negative `GOGO_E*`. `GOGO_MAX_HOST_PORTS` is gone (a VM still
    takes 4 host ports).
  - The `GOGO_E*` results are the shared error codes negated, so their
    numbers changed (`GOGO_EINVAL` -1 is -6, `GOGO_EREQUIRES` -4 is
    -10, `GOGO_EVERSION` -13 is -27, ...); code that uses the names
    needs no edit. `GOGO_ESTARTED` is `GOGO_ESTATE`, `GOGO_ESTART` is
    `GOGO_ENOTFOUND` (no `main`) or `GOGO_ENOMEM`, and the old name
    `GOGO_ECAPS` is gone.
- **Command line.** Every `gogo` command parses its flags the same way
  (book chapter 12): one or two dashes, flags before or after the other
  arguments. So an argument starting with `-` is a flag even after a
  file name: put it after `--`. The exceptions: `gogo config set` takes
  its value as it is (`gogo config set offset -5`), and `gogo run` takes
  its flags before the program only (what follows it is the program's
  own, `sys.Args()`). A usage
  mistake prints what is wrong before the usage.
- **Configuration keys.** `gogo config set` takes keys of `a-z 0-9 . _`
  only (upper case and `-` were allowed), refuses unknown keys under the
  gogo prefixes `wifi.`, `net.`, `fs.`, `code.` and `gogo.`, and checks
  the values of gogo's keys (book chapter 11). `get` and `rm` still take
  the old spelling.
- **`gogo config` store choice.** Without `-port`, `-local` or
  `$GOGO_CONFIG`, `gogo config` fell back to the local file
  (`~/.gogo/config`) when no board or several boards were connected, so
  a `set` could silently miss the board. Both cases are now errors (no
  board found / several boards found, with the ports); pass `-local` or
  set `$GOGO_CONFIG` to mean the local file (book chapter 11).
- **Exit statuses** of `gogo run` and `gogovm` (book chapter 12): a
  crash of `main` exits 4 and `actor.Exit(code)` in `main` exits with
  the code (0 before). A missing driver or mount exits 3 (was 1), a bad
  image still 2, and a `gogovm` usage error 1 (was 2).
- **Board code** (C, `platform/`; boards outside this repository must be
  edited): a generic driver is now an instance over the board's hardware
  layer (`PortDriver.hw`, design §5.11.1). `PortDriver` gained a last
  member `hw`: write driver tables with designated initializers (as
  every table in the repository now is), or add one more `NULL` to a
  positional one.
  `VmBoard.input` and `InputHw` are gone: list `INPUT_KBD_DRIVER(&hal)`,
  `INPUT_POWER_DRIVER(&hal)`, ... over `KbdHal`, `PowerHal`, `ImuHal`,
  `IrTxHal`, `IrRxHal`, `MicHal`, `SpkHal`, whose hooks take a `ctx`
  first. The `gpio` driver is `GPIO_DRIVER(&hal)` over a `GpioHal`.
  `VmBoard.mounts` lists `VmMount{prefix, be, ctx}` entries, not
  strings. Write `VmBoard` with designated initializers. A board boots
  its image with `gogo_board_main` (`runtime/board/board_main.h`) and
  speaks `gogo config` through `runtime/configsvc` over its own `KvHal`.

## Deprecated, removed in the next release

- `gogo monitor -t seconds` is `-timeout seconds`; `-t` still works and
  says so on standard error.
- `gogo config scan` and `gogo config stats` are `gogo device scan` and
  `gogo device stats`; the old forms still work, with a note.
- `gogo config set fs.flash.format yes` (format `/flash` at the next
  boot) is `gogo device format-flash`, which formats at once (the board
  restarts) and writes nothing to the configuration store. The key still
  works, with a note.

## VM

- **A forged crash report can set gogovm's exit status.** gogovm reads
  main's status from the crash report on standard error, and the report
  prints a function's name as the image has it. A hand-made image whose
  function name holds a newline followed by a report line for `main`
  sets the status, even if `main` then exits normally. The compiler
  never makes such a name; escaping it would cost VM core bytes.
- **Only `main` sets the exit status.** `gogovm` and `gogo run` exit
  with the main actor's status (book chapter 12); a crash of another
  actor, while `main` ends normally, still exits 0. *Workaround:* link
  the actors whose failure must fail the run to `main` (or have `main`
  monitor them and `actor.Exit` with a code).
- **`actor.Call` does not notice that the server died.** It waits out
  the whole timeout and fails with `errors.Timeout` (only a server that
  was already gone gives `errors.NoProc`, at once). *Workaround:* monitor
  the server and keep timeouts short; treat `errors.Timeout` as "maybe
  dead".
- **Timers outlive their creator.** A `SendAfter` aimed at another actor
  still fires after its creator exits and is then charged to nobody, so a
  child that is restarted over and over and sets a long timer each time
  piles up timers. *Workaround:* cancel such timers before exiting, or
  let the target set its own timers.
- **Stale replies use mailbox quota.** A `Reply` that arrives after its
  `Call` timed out stays in the mailbox, counted against the receiver's
  quota, until the next `Receive` or `Call` scans past it and drops it.
- **PIDs can be reused.** A PID is a 16-bit slot plus a 12-bit
  generation. After 4096 reuses of one slot, an old PID names the new
  actor in that slot, and a stale `Send`, `Kill` or `Link` reaches it.
  This matters only for long-running programs that keep PIDs of dead
  actors. *Workaround:* drop PIDs when their `DownMsg` or `ExitMsg`
  arrives; look services up by name.
- **Crash reports show only the PID slot.** `<pid N>` omits the
  generation, so a restarted worker can show the same number as its
  predecessor, and the zero PID prints as `<pid 0>`, like the main actor.
- **Linked victims look like the culprit.** When B crashes with `divzero`
  and a linked A that does not trap exits too, stderr shows `crashed:
  divzero in <B's function>` for A as well.
- **Some operations cannot be preempted.** Reductions are counted only
  at calls and backward jumps. Copying a large message, a GC, the mailbox
  scan of `Call`, storage calls and the TLS key exchange run to the end
  and stall every actor meanwhile. Storage calls take tens of ms on
  `/flash` (an erase) and occasionally hundreds on `/sd`; the TLS step
  takes about 0.2 s (see below). *Workaround:* keep messages small and do
  storage and HTTPS work in actors whose latency does not matter.
  *Planned:* storage calls become asynchronous: the file operation runs in
  a background platform task (on the ESP32-S3, a FreeRTOS task beside the
  VM) and its result comes back to the actor as a message, so a slow
  flash erase or SD write no longer pauses other actors. The same rule as
  the planned host network ports: helper threads only do or report I/O,
  and everything that touches actors stays on the VM thread, so the VM
  needs no locks.
- **Accounting is incomplete.** Links, monitors and pending `SendAfter`s
  are charged, but some VM-global resources are not: the per-actor extra
  record itself, registered names, the timer heap's slots, and a
  `SendAfter` whose creator has exited. `actor.Info(p).Memory` leaves
  these out too. The name table is bounded only by the number of names
  programs register.
- **The `bytes.Bytes` handle table is small on a board.** The whole VM
  has 64 off-heap handles and one actor may hold 16 (4096 and 256 on the
  host; the config key `vm.handles` raises them, to at most 1024 on a
  board and 65535 on the host). When
  the table is full, the VM first collects the holders; if that frees
  nothing, the allocation fails (`OutOfMemory`) and ports that need a
  handle refuse (a stream `Out` gets `Done{-1}`). *Workaround:*
  do not keep many large frozen `Bytes` alive in one program; on a
  board, `gogo device stats` shows the table's use (`bytes=N/64`, with
  `ports=N/16` and `subs=N/4` for the port and `net.Subscribe` tables).
- **Host and board memory limits differ by 64x.** An actor spawned
  without `SpawnOpt` may use 1 MB on the host and 16 KB on a board, so
  a program that passes under `gogo run` can die with `oom` on the board.
  *Workaround:* run with all of the board's limits, `gogo run --actor-memory 16K --memory 160K --max-actors 1024 --max-ports 16 --max-links 8 --bytes-handles 64/16 prog`
  (the host's tables are sized from them: with only the first two, the
  16384-actor table alone takes most of 160 KB), or test with
  `build/gogovm_mcu` (built by `make test-go`), the board build and the
  exact model: `GOGO_VM=build/gogovm_mcu gogo run ...`.
- **An actor holds at most 256 KB on a board** (65535 words: its word
  counts are 16-bit there, which keeps its record at 48 bytes); a larger
  `vm.actor_memory` or `MaxMemory` is cut to that. On the host the
  ceiling is 1 GB, but one slice, byte slice or string is at most 2^26 - 1
  words (256 MB, its header's size field): a bigger `make` or `append` is
  `OutOfMemory`. One message is at most 65535 words (256 KB) on every
  platform; a send of a bigger value fails with `OutOfMemory`.
- **A message of many off-heap `Bytes` can leave its receiver a little
  over its limit.** The quota counts the holder-list growth one more
  handle causes; a message that brings many handles at once can double
  the list again on receive. The overshoot is the list's growth, at most
  its size at the per-actor cap (`vm.handles`' M: about 1 KB at the
  host's 256; none at a board's 16, where the list starts at its cap),
  and memory-safe; port handoffs count the same way.
- **The VM's limits are read at start.** `vm.actor_memory`, `vm.memory`,
  `vm.max_actors`, `vm.max_ports`, `node.max_links`, `vm.handles` and
  `code.mem` take effect at the next VM start (restart the board after
  `gogo config set`); a running VM keeps its own. `vm.memory` on a board
  can only lower the 160 KB arena.
- **`actor.Limit` needs image format 15.8.** It is a new builtin, not a
  field of `actor.ProcInfo` (a field would have made every program that
  calls `actor.Info` 15.8); a program that calls it does not run on
  firmware before 15.8.
- **`gogo/supervisor` supervises one child.** There is no multi-child
  `one_for_one` strategy. *Workaround:* one supervisor per child.
- **Small costs on hot paths.** Sending to a dead or stale PID walks the
  message once (to check it).
- **Values with heavily shared parts.** `println`, every port command and
  `port.Open` walk the whole value (to depth 16) without a visited set,
  so a part reachable by many paths is walked once per path. The walk
  stops after 65536 terms, so it takes at most a fraction of a
  millisecond on the host: `println` then ends the value with ` ...`,
  and a port command or `port.Open` ends the actor with `badcode`. On a
  board a value without shared parts never reaches the limit (an actor
  holds at most 65535 words); on the host, where an actor may hold far
  more, a flat value of more than 65536 terms reaches it too (a
  `println` of a 100000-element slice ends with ` ...`). Moderate sharing
  in a large value reaches it on both: 5000
  elements that each hold the same 16-element slice take about 85000
  visits. *Workaround:* send ports flat values; print the parts of a
  shared structure separately.

### Hot loading

- **No signatures and no permissions.** Any actor that can hand
  `code.Load` some bytes can run code with the same rights as the main
  program. Its requirement names say what the firmware must provide, not
  what a module may do. Function, type and jump indexes are checked when
  used (`badcode`), not when loaded. *Workaround:* load only images you
  trust, fetched over an authenticated channel (`https://` from a server
  you control, as `examples/hotload_net` does; never plain `http://`).
  *Planned:* signed images: an Ed25519 signature over the image, checked
  by `code.Load` against a public key in the firmware, with the PSA
  crypto the TLS port already links (`TlsHw`).
- **Modules live in RAM only.** There is no automatic loading from flash
  at boot and no execute-in-place from flash. *Workaround:* store the
  module (`gogo push`, or `fs.WriteFile`) and load it with `fs.ReadFile` +
  `code.Load` at start.
- **No `sys.Has`.** A program cannot ask which drivers and mounts the
  firmware has before loading; it learns the first missing one from
  `code.Missing` after `Unsupported`, and an absent driver from
  `port.Open`'s `errors.NotFound`.
- **`code.Missing` names one requirement.** Only the first missing name
  is reported, and at most 32 bytes of it.
- **Stale module values alias after 256 loads.** A value from an
  unloaded module carries 8 generation bits; after 256 more loads into the
  same slot it resolves again, to the new module (still memory-safe, but
  not `badcode`). Slots are reused round-robin, so this takes about 1800
  loads with all slots free.
- **Memory.** Each loaded module takes a 64-byte table entry (about
  0.5 KB for 8 modules). A module running in place from a `Bytes` is
  counted in `code.mem` and also, until their next GC, against every
  actor that still holds that `Bytes`. The boot image (module 0) is not
  reachable through `gogo/code`.

## Compiler and language

- **Not supported (out of the design).** `goto` is rejected (labeled
  `break` and `continue` cover its uses). Channels, `go`, `select`,
  generics, `recover` and `unsafe` are outside the design.
- **Maps differ from Go's in order, speed and keys.** `range` over a map
  goes in the order of its keys (numbers by value, strings byte by byte,
  structs field by field, a `uint32` or `uint64` key by its signed bit
  pattern), not Go's random order; code that relies on Go's
  randomization sees a fixed order. A map is a sorted slice: a lookup is
  O(log n), but an insert or a delete moves the entries after it, O(n),
  and a large map is slower than Go's hash map (see `gogo/maps`). Keys
  may not hold pointers, maps or funcs (`map with key type ...: a pointer
  key`); a key that holds one at run time, at any depth (an interface key,
  or an interface field of a struct key), ends the actor with a panic
  (`hash of unhashable type`, as Go does for a map or a func: Go accepts
  the pointer), and so does a slice held directly by an interface key; a
  slice deeper inside such a key is compared element by element (Go
  panics). NaN keys behave as in Go (each insert adds an
  entry, a lookup never finds one); they range first, and if a loop body
  changes the map while ranging over NaN keys, the NaN keys it has not
  reached yet are skipped (Go would produce them). A map sent to
  another actor is copied with its entries (Go would share it). A nil map
  reads as empty and a write to it ends the actor with `actor.Nil`, not a
  `panic`. A nil map or a nil pointer in an interface is the nil
  interface value (Go's interface would hold a typed nil): `x != nil` is
  false, and a type switch does not match its type. `println` of a map
  prints `map[k:v ...]`, of a nil map `nil`.
- **Package-level variables are read-only.** A package-level `var` is set
  when the program is built and cannot be written (`package-level
  variable x is read-only`); mutable state belongs in an actor. Its
  initializer must be static: constants, literals, conversions, other
  package-level variables and calls of single-`return` functions such as
  `errors.New`, but no `make`, no `&T{}` and no arithmetic on a function's
  parameters (`package-level variable x: ... is not static`). A slice of
  one shares its constant array: `t := table; t[0] = 1` ends the actor
  with `badcode` at run time, and so does `copy` into it or an `append`
  that would fill its spare room. *Workaround:* copy first,
  `own := append([]int{}, table...)`. Sent in a message, such a value
  stays read-only in the receiver, and a struct or slice of one cannot be
  sent to another module (`BadMsg`): an error from `errors.New` held in a
  package-level variable is one; return or send `errors.New(...)` itself
  to cross modules.
- **`gogo/fmt` differs from Go's.** It has no reflection: inside a
  struct, slice or map a field's `String` or `Error` method is not called
  and a `uint32` or `uint64` prints signed; a pointer below the top
  prints `&` and its target (Go prints an address); `%p` and `%T` are not
  supported (`%!p(...)`), and a bad verb or an extra operand names only a
  basic type (`?` otherwise); `%s` of a `[]byte` prints its elements and
  `%q`, `%x` and `%X` of one print `%!x(?=[104 105])` (convert with
  `string(b)`), and `%x` of a value of a named integer type prints
  `%!x(?=5)` (convert it to its integer type); `%+v` and `%#v` print a
  struct as `%v` does, `{1 2}`, without field or type names (Go:
  `{X:1 Y:2}`, `main.P{X:1, Y:2}`); `Sprint` puts a space between an
  operand of a named string type (`type S string`) and its neighbour,
  where Go puts none (it sees the string kind); width and precision count
  bytes of a string; `Errorf`'s error is a value (two with the same text and
  wrapped error are equal) and an error it wraps with `%w` cannot be sent
  to another module (its type is gogo/fmt's own). `Scan` and the `Fprint`
  family are missing (no `io`). Formatting costs code: a program that
  calls `Sprintf` carries the whole formatter and the `error` and
  `Stringer` dispatchers (about 19 KB of image, with gogo/errors and
  gogo/strconv).
- **`gogo/errors` differs from Go's.** Two errors from `errors.New` with
  the same text are equal (errors are values, compared by what they
  hold; Go's are equal only to themselves). There is no `errors.As`
  (no reflection) and no `errors.Join`.
- **A shared package's struct types cannot cross modules.** A program
  and a hot-loaded module (or two modules) that import the same package,
  your own or a `gogo/` library such as `gogo/http`, each compile their own
  copy of it, so its struct types are different types in each. Sending
  one across modules fails with `BadMsg` at run time, not at compile time
  (book 14.6). Only the system packages' reserved types (`actor.Request`,
  `net.Up`, `fs.Info`, ...) cross. *Workaround:* send basic values,
  strings or `Bytes`, and rebuild the struct in the receiver.
- **`sys.Args()` has no program name.** It returns only the arguments
  after the program (Go's `os.Args[1:]`), so there is no element 0 to tell
  a script its own name. Boards get an empty slice: nothing starts a
  program there with arguments. A `libgogo` host passes arguments by
  answering the `config_get` hook for the key `"\x00args"` (book 18; the
  value is split at NUL bytes). Every `sys.Args()` call copies the
  arguments into the actor's heap: call it once and keep the result; a
  very large argv (hundreds of KB) runs the actor out of memory.
- **A spawned closure copies what it captures.** Within one actor,
  closures share the variables they capture, as in Go (a variable written
  after a closure captured it lives in a box, one allocation at its
  declaration). A closure passed to `actor.Spawn` is copied into the new
  actor with what it captures, so a variable it changes there is the
  child's own (Go's goroutines would share it).
- **Multi-value expressions only in assignments.** `println(f())` with a
  multi-result `f` and `var a, b = f()` fail with `multiple values not
  supported`; use `a, b := f()`. A multi-result call as a statement or
  in a `defer`, `actor.ReceiveTimeout(0)` included, is fine: its results
  are dropped.
- **Pointers: no interior pointers, a copy after a send.** `&s[i]` and
  `&v.F` are rejected (`interior pointer ... not supported`), and so is a
  pointer method called on a slice element or a field (`s[i].M()`,
  `v.F.M()`: it would need `&s[i]`), or promoted from an embedded field.
  *Workaround:* keep pointers in the slice or field (`[]*T`, a `*T`
  field), or write the element's field directly, `s[i].X = v`. Pointers
  to arrays (`&a`, `*[N]T`) and embedded pointer fields are rejected too.
  A pointer sent to another actor (in a message, or captured by a spawned
  closure) is copied with its target, as a slice's array is: the receiver
  gets its own target, so a write there is not seen by the sender (Go's
  goroutines would share it). A nil pointer dereference ends the actor
  with the reason `actor.Nil` (crash kind `nil`), not a `panic`, and
  there is no `recover`. `println(p)` prints the target after `&`
  (`&{1 2}`, `&5`), not an address. A local variable whose address is
  taken lives in a box (one allocation at its declaration), and `*p` of a
  pointer to a struct copies the struct (one allocation). A named type
  other than a struct or a basic type (`type L []int`) cannot be
  declared, so it has no methods. The methods of a type in a hot-loaded
  module are that module's own, like its types.
- **Arrays: no `==`, not in `any`, hidden copies.** `a == b` on arrays
  is rejected until composite values can be compared (`comparison of
  arrays not supported yet`): compare the elements. An array cannot go
  into an `any` (`array in interface not supported`), so it is not a
  message on its own: send a slice or a struct with an array field. Only
  an array variable can be sliced (`p.A[:]` is rejected: copy the array
  into a variable first). Named array types (`type G [9]int`) are not
  supported. Like Go, gogo copies an array on every assignment, argument,
  result and field or element store, which allocates N elements each time
  (a literal or a call result is not copied again); pass `a[:]` to avoid
  it. A write through an array in a field or an element
  (`p.A[i] = v`, `m[i][j] = v`) also copies that array, and rebuilds the
  structs on its path, so it costs O(N) rather than O(1). Like a slice,
  `println(a)` prints the elements (`[1 2 3]`; Go's `println` rejects
  arrays).
- **`defer` is static and runs on normal returns only.** A `defer`
  inside a loop is rejected (`defer inside a loop not supported`): move
  the loop body into a function. A crash, a `panic` (Go runs the
  deferred calls while it unwinds) or `actor.Exit` ends the actor
  without running the deferred calls (its ports and files are closed
  anyway). `defer
  f(g())` with a multi-value `g` is rejected. Each `return` compiles the
  deferred calls again, so many returns after many defers make the
  function's code larger.
- **Embedded fields: no pointers, no promoted method expressions.**
  Embedding `*T` is rejected (`embedded pointer field not supported`). A method expression of a
  promoted method (`Outer.M` for a method of an embedded field) is
  rejected (`method expression of a promoted method not supported`).
  *Workaround:* a method value `x.M`, or `Inner.M(x.Inner)`. The methods
  of an embedded gogo type (`bytes.Buffer`, `bytes.Bytes`, `fs.File`) are
  not promoted (`method M of an embedded gogo/bytes type not
  supported`): call them on the field, `w.Buffer.WriteByte(b)`. Other
  promoted methods count toward implementing an interface, an embedded
  interface's included; those of an embedded gogo type do not.
- **Every field write allocates.** Structs are immutable in the VM, so
  `v.X = v` on a struct variable (not through a pointer, whose target is
  written in place) builds a new struct (2 + fields words) for each struct on the
  path (`a.b.c = v` builds three) and the old one becomes garbage: the
  same cost as rebuilding it with a literal by hand. On the host a write
  to a 3-field struct in a tight loop costs about 30 ns more than an `int`
  increment, and more garbage means more frequent collections. In a hot
  loop over a large struct, keep the changing values in local variables
  and write them back once. Each level of a deep path also takes as many
  registers as its struct has fields, so a long path through very wide
  structs can fail to compile (`needs more than 255 registers`).
  *Workaround:* copy an inner struct to a local, write it, store it back.
- **A slice on a field path is read before the right-hand side runs.** In
  `a.S[1].X = f()`, gogo reads `a.S` before calling `f`, so if `f`
  replaces `a.S` the write goes to the old array (Go leaves this order
  unspecified; `s[i] = v` behaves the same way in gogo).
- **No field assignment through a map index.** `m[k].X = v` is not valid
  Go either: read the struct, write it, store it back. A field of a
  pointer's target can be written (`p.X = v`, `f().X = v` for a `f` that
  returns a pointer).
- **Some values cannot go into `any`.** Func values (no type id) are
  rejected. Func values cannot be compared, not even with `nil`.
  *Workaround:* wrap a func value in a struct.
- **Boxed values cost an allocation.** A sized integer (`byte`, `rune`,
  `uint32`, ...) or a named basic value (`type Celsius int`) put into an
  interface is boxed in a one-field struct (a constant one is not), and
  an `int64` in a `K_INT64`; taking it out unboxes it. The box of your
  own named type is a type of your module, so like your structs it
  cannot be sent to another module.
- **`fs.File` and `actor.PID` share a type tag.** `fs.File` cannot be a
  type switch case, and an `fs.File` in an `any` is seen as a PID.
  *Workaround:* wrap it in a struct.
- **No dead-code elimination.** Importing a library compiles all of it
  into the image (`gogo/strings` costs about 440 words, `gogo/strconv`
  about 360), which matters on boards with little flash. The exception
  is a file marked `//gogo:ondemand`, whose functions are compiled in only
  when the program uses them: strconv's `FormatUint` (about 130 words) and
  `ParseUint` / `ParseInt` (about 430 together) are in one.
- **Requirement inference is a heuristic.** A constant `"/sd"` path
  passed to `gogo/fs` makes the image require `/sd`, even if the program
  only probes for a card (there is no directive for mounts). *Workaround:*
  pass the path through a variable (as `examples/audiohw` does). A
  constant `port.Open` requires its driver unless the package marks it
  `//gogo:optional` (libraries and `package main` alike).
- **A non-constant driver name escapes the load-time check.** The
  compiler cannot see it, so the image does not require it and a board
  without the driver loads the image anyway (the open then fails at run
  time). The build warns at each such call with its position; name the
  driver and use `//gogo:optional` instead.
- **Requirements count unreachable code.** Every compiled package's
  constant opens count, reachable or not: an http client also requires
  `tcp_listen`, because the server code is compiled in.
- **Old images need a rebuild once.** Images of the single-number formats
  (up to 14) do not load on a v15 VM; from 15.0 on, a VM runs every older
  minor of its major.
- **Interface calls cost a type check per candidate.** A method call on
  an interface value tests the value's type against each type of the
  program that has the method, in declaration order, before calling it;
  a program with many such types pays for the chain (and its image holds
  one dispatcher per interface method called).
- **`errors.OK` in an `error` is not `nil`.** `gogo/fs` and `gogo/code`
  return an `errors.Code` (`fs.Err`, `code.Err`), whose success is
  `errors.OK`, the code 0; converted to `error` it is a non-nil value,
  like a typed nil pointer in Go, so `return fs.WriteFile(p, b)` in a
  function that returns `error` would turn every success into a failure.
  The compiler rejects a call's `errors.Code` result returned or stored
  directly as an `error` (a `return`, a `var` or an assignment:
  `errors.Code result used as an error`); passing one to compare it,
  `errors.Is(fs.Remove(p), errors.NotFound)`, an explicit
  `error(fs.Remove(p))` and a `Code` variable converted later are
  allowed. *Workaround:* `if c != fs.OK { return c }; return nil`. The library functions that return `error` return `nil`
  on success.
- **The VM's own types do not implement your interfaces.** The methods
  of `bytes.Bytes`, `bytes.Buffer` and `fs.File` are builtins, which a
  dispatcher cannot call: putting one (or a struct that embeds one) into
  an interface whose methods it would provide is a compile error (`bytes.Bytes as a
  main.Stringer not supported`), and `x.(Stringer)` (or a type switch
  case `Stringer`) on an `any` holding a `Bytes`, or a struct that
  embeds a `bytes.Buffer`, is false, where Go says true. *Workaround:* wrap the value
  in a struct of your own with the methods.
- **Interface values of your types stay in your module.** A hot-loaded
  module and its loader dispatch only over their own types (the closed
  world that makes dispatch work), so a value of your type, inside an
  interface or not, cannot be sent to another module (`BadMsg`, or a
  refused `code.Spawn`).
- **`panic` of an error prints `Error()`**, as Go does, but a
  `fmt.Stringer` value prints as a value (Go prints its `String()`).
- **Panic texts differ from Go's in places.** A failed `x.(T)` says
  `interface conversion: interface {} is not string`, without the
  dynamic type Go names (`interface {} is int, not string`);
  `panic(nil)` prints `panic: nil` where Go 1.21+ raises a
  `*runtime.PanicNilError`; a method call on a nil interface value is
  reported in the interface's dispatcher (`crashed: panic in
  Shape.Area`), not in the caller.
- **`panic` ends the actor; there is no `recover`.** A panic is a crash
  with the reason `actor.Panic`, for links, monitors and supervisors to
  handle. Its value prints as `println` prints it (a struct as `{1 2}`, a
  named basic value inside an interface as `{5}`),
  where Go prints a struct's type and address.
- **`==` on interfaces holding slices does not panic.** An `any` holding
  a slice equals nothing, not even itself: Go gives false for slices of
  two types and panics for slices of one (`comparing uncomparable type`).
  A slice deeper inside, in a struct field of an `any`, compares element
  by element, as arrays do, and without its element type
  (`S{[]int(nil)} == S{[]string{}}` is true).
- **`==` has a depth and a work limit.** `==` and `switch` on structs,
  arrays and interface values walk both values in one VM instruction,
  without preemption, like a large message copy. Nesting deeper than 64
  levels (other than through a struct's last field or an array's last
  element, which loop) ends the actor with `badcode` where Go answers;
  so does a compare that visits more than 65536 terms, which values
  sharing parts reach quickly (`x = N{x, x}` in a loop: the walk follows
  paths, not objects), and, on the host, plain large values do (`x == y`
  for two `[70000]int` arrays). Each level takes about 100 bytes of the VM's
  stack. *Workaround:* compare the fields of a deep value yourself.
- **`println` of a slice differs from Go.** gogo prints the elements,
  `[1 2 3]`; Go's `println` prints `[len/cap]address` (only
  `fmt.Println` prints elements).
- **Library differences from Go.** `strconv` handles bases 2 to 36 only
  (no prefixes, no underscores), `Atoi` overflows at 32 bits, and
  `ParseInt` / `ParseUint` with bit size 0 mean 32 bits; their errors
  are `errors.BadFormat`, `errors.Overflow` (with the value clamped, as
  in Go) or `errors.BadArg` (a bad base or bit size), not
  `*strconv.NumError`s;
  `strings.TrimSpace`, `ToLower` and `EqualFold` are ASCII-only and
  `Split(s, "")` splits into bytes; slices grow by doubling from a
  capacity of 4.

## Floating point

- **Every float is a heap object.** A `float32` takes 8 bytes and a
  `float64` 12, and every arithmetic result, conversion or `ParseFloat`
  allocates one (constants live in the image and cost nothing). A tight
  float loop therefore runs the garbage collector often. *Workaround:*
  keep inner loops on integers (fixed point) where it matters.
- **`float64` is software on the boards.** The ESP32-S3's FPU is single
  precision: `float32` arithmetic runs on it, `float64` in libgcc's
  software routines. Dispatch and boxing dominate both, so the gap is
  about 2x per operation, not more (on the StickS3 a loop of 20000 `x = x*0.999 + 0.25` takes 313 ms in `float32`, 409 ms in `float64` and 226 ms as the same loop on `int`s). Both are software on a chip
  without an FPU (the RP2040). *Workaround:* use `float32` for sensor
  maths.
- **Firmware option.** Floats need firmware built with `GOGO_FLOAT`
  (requirement `float`, image format 15.2). On a firmware without it, an
  image that can make floats (a float constant, a conversion of an
  integer, `math.Inf`, `math.NaN`, `ParseFloat`) is refused at boot (`gogo: image requires float,
  which this VM lacks`) and `code.Load` returns `Unsupported`. `gogo
  firmware` builds it in only when an image needs it (or `-with float`),
  so a module that needs floats and is loaded later needs `-with float`.
- **Implementation-defined conversions.** Go leaves `int(f)` of a NaN or
  an out-of-range `f` to the implementation; gogo gives 0 for NaN and
  the nearest end of the range otherwise (Go on arm64 does the same; on
  amd64 Go gives the minimum value). With a 32-bit `int`, `int(3e9)` is
  2147483647. `byte(f)` is the low 8 bits of that `int` (`byte(300.7)` is
  44, `byte(-1.5)` 255).
- **A small `gogo/math`.** `Sqrt`, `Floor`, `Ceil`, `Trunc`, `Round`,
  `RoundToEven`, `Abs`, `Copysign`, `Signbit`, `Inf`, `NaN`, `IsNaN`,
  `IsInf`, `Max`, `Min` and the constants only: no `Pow`, `Exp`, `Log`,
  trigonometry, `Mod` or `Float64bits` yet.
- **`strconv` float differences from Go.** `ParseFloat`'s error is
  `errors.BadFormat` for bad syntax (`f` 0) and `errors.Overflow` for an
  out-of-range value (`f` ±Inf, Go's `ErrRange`), not a
  `*strconv.NumError`. `FormatFloat` has no
  `'x'` / `'X'` (hexadecimal) format: it gives `%x`, as Go does for an
  unknown format. Both are exact and the same on the host and the boards
  (gogo's own code, not the C library's).
- **Float equality in `switch` and `==` follows IEEE 754.** A `case`
  with `NaN` never matches, and `-0` matches `0`, as in Go.

## Sized integers

- **Boxed in `any`.** An `int8`, `int16`, `int32` (`rune`), `uint8`
  (`byte`), `uint16`, `uint32`, `uint` or `uint64` value in an interface
  (and so in `actor.Send`) is a one-field struct of its own type, an
  allocation unless it is a constant (see "Boxed values cost an
  allocation").
- **`println` of a struct, slice or array holding unsigned 32- or 64-bit
  values is rejected.** `println` prints bare unsigned values correctly,
  but inside a composite value the static type is gone and they would
  print as their signed bit patterns. *Workaround:* print the fields or
  elements one by one. The check is static: a struct with such fields
  inside an `any` (`var v any = Q{0xFFFFFFFF}`) is printed, signed
  (`{-1}`). A `println` with a `uint64` argument after the 30th is
  rejected too (`B_PRINTLNU`'s mask has 30 bits).
- **`uint32` division, ordering and `>>` cost more than `int`'s.** They
  widen both operands to 64 bits and use the `int64` ops (two or three
  more instructions, and an allocation for values from 2^30 up, which
  are heap boxes as large `int`s are); `+ - * & | ^ <<` and `==` cost the
  same as on `int`. `uint64` costs what `int64` does. *Workaround:* in a
  hot loop on values below 2^31, `int` is cheaper.
- **No `uint64` or `int64` index.** Indexes and slice bounds may be any
  integer of up to 32 bits; convert a 64-bit one with `int(i)`.
- **Image format 15.3.** Division, remainder, ordering or `>>` on a
  `uint64`, arithmetic on `int8`, `int16` or `uint16`, the `uint32` ops
  above and `println` of an unsigned value need firmware of image format
  15.3 or later; `byte` and `int32` arithmetic and `uint32` `+ - * & | ^
  <<` do not. `gogo/strconv`'s `FormatUint` / `ParseUint` / `ParseInt`
  are written without them, so importing it does not raise an image's
  format.
- **Float conversions out of range** follow Go on arm64: `uint64(f)` is 0
  for NaN and negative values and 2^64-1 from 2^64 up; `uint32(f)` is the
  low 32 bits of `int64(f)` (4294967295 for `f` = -1.0, 1410065408 for
  1e10); `int8`, `int16` and `uint16` are the low bits of `int(f)`,
  like `byte`.
- **`uint` is 32 bits**, as `int` is; Go code that assumes a 64-bit `uint`
  overflows sooner.

## Networking, TLS and HTTP

- **No network on the host.** `gogo run` has no network; the
  simulator in `tests/net` is reachable only from Go tests. Network
  programs must be tried on a board or through a Go test.
- **TCP allows few writes in flight.** The window is one segment; a
  connection takes one `Out` in flight and one queued (an `Out` whose
  `Done` is still owed counts). A third `Out` before a `Done` arrives is
  refused with `Done{-1}` and not sent; the connection carries on. A
  naive echo server that ignores `Done{-1}` loses data under load.
  *Workaround:* count `Done` events and buffer
  (`docs/book/examples/10-tcp-echo` does).
- **Small writes need a `Bytes` handle.** A small `Out` is copied into
  a port-held handle (for retransmission); if the VM's handles (64 on a
  board, `vm.handles`) are all taken, it is refused with `Done{-1}`.
- **The microphone does not send `port.Closed`.** tcp, tls and host
  ports (`gogo_port_close`) do, but a microphone hardware failure is not
  reported: its owner learns of it only by silence. The message and its
  meaning (the port ended by itself; close it) are fixed, so it can
  start sending one without a change to programs.
- **Losing the link or address ends every connection** with
  `Closed{NoRoute}`, and so does the exit of the `netif` owner (the DHCP
  actor). A crashed DHCP actor therefore drops all connections.
- **Network protocol shortcuts.**
  - A packet to a LAN host that is not in the ARP cache is dropped while
    the address is resolved, so the first packet to a new host is lost
    (applications and DNS retry).
  - TCP initial sequence numbers are not cryptographically random
    (mitigated by exact-match resets and challenge ACKs).
  - Orphaned connections (closed by the owner after `Shutdown`) are reset
    after 20 s (`GOGO_NET_ORPHAN_MS`), so a peer slower than that loses
    the end of the response.
  - Incoming connections may not take the last free connection slot, so
    a device serving only incoming traffic has at most 3 at once.
  - `port.Close` after `Shutdown` finishes the close as an orphan; it
    does not reset.
  - UDP datagrams over the receiver's quota are dropped (and counted).
  - Of the 4 `net.Subscribe` slots, DHCP and DNS take one each.
  - During fast link flaps, `Up`/`Down` events are replayed as the
    current state, so a subscriber may see `Up`, `Up`.
  - The IP source filter does not reject 127/8.
  - DHCP retries renewals after 1 s at the soonest (RFC: 60 s), ignores
    options 58 and 59, and a lease time of 0 gives `Up` then `Down`.
- **HTTP limits.** The one-shot `http://` client (HTTP/1.0) refuses
  chunked responses; https requests and `http.Client` decode them. The
  server does not accept chunked request bodies; the request head is at
  most 2 KB and the body 8 KB. A request with bare LF line ends waits for
  the read timeout. Responses are read into memory whole (8 KB by
  default, up to 64 KB with `http.Do`); there is no streaming download
  yet (uploads stream). Request actors run with 2 x body limit + 6 KB
  (22 KB by default; about 134 KB with a 64 KB body limit). `http.Serve`
  has no error result: if it cannot listen it kills the linked caller
  with exit code 69, `http.ListenFailed` (`actor.Fail(errors.Busy)`),
  whatever the cause.
- **TLS is 1.2 only.** mbedTLS's TLS 1.3 key exchange cannot be sliced.
  A server that requires 1.3 gives `Closed{TLS}`.
- **One TLS connection at a time** (`GOGO_TLS_MAX` is 1). An
  `http.Client` holding its idle connection makes every other https
  request fail at open until `CloseClient`. *Workaround:* share one
  Client, or close it when done.
- **The TLS handshake stalls the board.** The ECDHE step cannot be split:
  on the StickS3 it takes about 204 ms with X25519 (about 300 ms with
  P-256), during which no actor runs. RSA signature checks are one step
  each too (not measured on a board). The rest of the handshake is sliced
  (mostly 11 to 26 ms, a few up to 50 ms) and a whole handshake takes
  about 2.5 s on the StickS3. Anything real-time (audio recording to a
  card, for example) suffers during a handshake. Removing the stall needs
  a patch to mbedTLS's TLS 1.2 client (interruptible key generation and
  agreement, with P-256, whose multiplication is restartable).
- **TLS memory.** The owner is charged about 23 KB per connection; the
  peak is about 34 KB. The difference (the peer's chain, key exchange
  state) is uncharged but bounded and freed when the handshake ends.
- **Keep-alive races.** A server that closes an idle connection just as
  the Client sends a request makes `ClientDo` fail, even for a GET: the
  Client never resends a request whose bytes went out. *Workaround:*
  retry idempotent requests in the caller.
- **TLS closes cost a round trip.** The tls port resets a connection
  closed before its `close_notify` went out, so `gogo/http` waits up to
  1 s (`LingerMs`) for the peer's close after a response; a one-shot
  https `Get` returns about one round trip later than it could.
- **The trust-anchor rule is stricter than browsers.** On the boards, a
  presented root that matches a bundle root is refused if it is expired
  or not yet valid, and only the last certificate sent is matched against
  the bundle, so a chain with an extra root after the cross-signed one
  fails.
- **A raw `tls` port does not detect truncation at a record boundary.**
  A FIN there is `Closed{Normal}`, as in Go's `crypto/tls`. `gogo/http`
  frames every https response and fails short ones; programs using the
  port directly must frame their own data.
- **Host only:** the unix build re-reads and parses the CA bundle for
  every TLS dial (about 200 KB of heap with the system bundle).
- **The wall clock has no access control.** Any code may call
  `sys.SetTime`; the only guard is the build-time floor.
- **Wi-Fi power saving is off** on the boards (fast responses, shorter
  battery life). After every reset the first association may be refused
  (`wifi: access point busy (reason 208)`); it connects about a second
  later.

## Distributed actors (`gogo/node`)

- **Node links are not encrypted.** The handshake proves the shared token
  (HMAC-SHA-256 challenge-response, the token never sent) and every frame
  carries a MAC under a session key, so frames cannot be changed, injected
  or replayed; but anyone on the network path can read them, and a
  captured handshake allows an offline guess of a weak token.
  *Workaround:* a trusted network and a long random token. Encryption
  needs a TLS *server* port (or an AEAD on the frames), which gogo does
  not have yet.
- **A star only.** Boards reach each other through the server; there are
  no direct board-to-board links yet, and a lost server cuts every board
  off (monitors and links fire with `NoConnection`).
- **The server node is a host VM** (`gogovm --host-net` / `GOGO_NET=host`):
  an embedding program cannot be one yet. Under `GOGO_NET=host` the host
  VM has `tcp` and `tcp_listen` only (no `udp`, DNS, DHCP or `tls`: the
  `tls` port still runs on the simulated stack), so addresses are
  numeric; `tcp_listen` binds every interface of the computer and `tcp`
  connects anywhere.
- **Boards per server: about 1380 at most.** A host VM serves 512 boards
  by default (`node.max_links`, `vm.max_ports`, or `gogovm --max-links` /
  `--max-ports`, each at most 2048); a board link costs the server one
  port and one file descriptor. The server's node agent has a memory
  limit that follows the link cap (and `vm.actor_memory`), so it no longer
  stops a server near 930 boards; the 65536 PID slots do: ports and remote
  PIDs (each board's door, each caller) share the 4096 above 0xF000, so
  1390 ports and 1380 links is about the most where every board can call
  the server (`make test-dist-scale`). With more links than that, boards
  still join but calls go unanswered once the remote PIDs run out. A
  board's own links cost it two ports each (its MCU stack cannot hand a
  TCP connection to the node port).
- **Joining needs retries.** A server takes *L* / 32 connections into
  their handshake at once (16 on a host, 2 on a board; at most 64) and
  answers the others `errors.Busy`; `node.Connect` makes one attempt, so a
  board retries with backoff and jitter (book 20.3). Anyone who can reach
  the port can keep those slots busy (each for up to the handshake
  timeout, 5 s), delaying joins, though not exhausting the server's ports
  or memory. `Connect`'s errors overlap: `Busy` also means this VM is a
  node already, and `Refused` covers a wrong token, a full server and a
  server that is not listening.
- **Tables sized by the link cap** *L*: 48 + 2*L* remote PIDs known at once
  (and at most 4096 minus the port cap: remote PIDs live between 0xF000
  and the port PIDs), 48 + 2*L* local PIDs given out (dead ones are
  reused), 16 + 2*L* remote links and monitors (a full table answers with
  `NoConnection`), 8 + *L* remote Calls in flight (one more fails at once
  with `errors.NoConnection`);
  on a board (*L* = 8) 64, 64, 32 and 16. A full remote-PID table evicts
  the least recently used entry that no link or monitor names: a remote
  PID kept without a monitor then goes stale (sends to it are dropped,
  and it no longer equals a fresh copy). *Workaround:* monitor the remote
  PIDs you keep. A message that needs one more entry when none can go is
  dropped.
- **Peers can fill the watch table.** Monitors from peers are not
  deduplicated or capped per peer, so a peer can fill the remote links and
  monitors table (peers are trusted alike).
- **A message of many empty slices may be refused.** The decoder refuses
  a term that would take more than two heap words per byte of encoding;
  a `[]any` of many empty slices is over that bound, and is dropped as
  bad.
- **Who can reach what.** The PIDs a node gave out are reachable by every
  peer, not only by the one they were sent to.
- **Not across nodes:** `actor.SendAfter` to a remote PID is `BadMsg`,
  `actor.Info` reports 0, a remote exit reason has no function name, the
  sharing of values inside one message is not kept (two fields naming one
  slice arrive as two copies), pointers and maps do not cross, and a Ref
  sent back to the node that made it is dropped there.
- **Own structs** are matched by qualified name, field count and field
  kinds (a shape hash the compiler stores with the type's name). Images
  from a compiler before the hash match by name and field count only; a
  mismatched definition then gives the receiver values of the wrong kind,
  which may misbehave where it uses them. The hash is stored only in
  programs that import `gogo/node`: a module compiled apart (hot loaded)
  that sends its own struct to a remote PID it was handed sends it
  without one, matched by name and field count only. Share message types
  through one package.
- **Drops are silent.** At most once means a message can be lost (a full
  link queue, a full table, a receiver over its memory limit) without the
  sender knowing; the link stays up (frames carry their number, gaps are
  accepted). The counters (`dist_stats`) are visible to C only.
- **Remote link and monitor notes are made when they fire.** The VM
  reserves the memory of a local link's or monitor's note in advance; a
  remote one is allocated when it fires, so under memory exhaustion it
  can be lost.

## Storage

- **The LittleFS host test needs a firmware build first.** The
  repository does not carry LittleFS; `make test-lfs` uses the copy the
  ESP build fetches and is skipped (with a note) until one has run.
- **A `/flash` that failed to mount is still listed.** The board keeps
  it in its mounts and every call under it is `NotMounted`, as before;
  only the order of checks differs (a `Write` open of a path already
  open, or a fifth open, reports `Busy` or `TooMany` before
  `NotMounted`).

- **Storage calls are synchronous.** Every `gogo/fs` call stops all
  actors until it finishes (see "Some operations cannot be preempted").
- **At most 4 files open** in the whole VM (`TooMany`).
- **`Close` does not report commit errors.** `File.Close` has no result.
  *Workaround:* call `Sync` before `Close` when you need to know.
- **`ReadDir` order is unspecified**, and differs between backends.
  *Workaround:* sort the names.
- **Name and path rules.** Names are 1 to 32 bytes of `A-Z a-z 0-9 . _
  -`, paths at most 96 bytes. On `/sd`, names longer than 64 characters
  (`FF_MAX_LFN`) cannot be opened; files created on a PC with other names
  are listed by `ReadDir` but give `BadPath` on open. Operations on a
  mount root, on `/`, renames across mounts and renames into a
  directory's own subtree are `BadPath`. A path through a file is
  `NotFound` on `/sd` but `NotDir` on `/flash`.
- **Case handling.** `/sd` is case-insensitive and `/flash` is not. The
  host backend (`gogo run --fs-root`) follows the host file system: on a
  case-insensitive host a rename that changes only case gives `Exists`.
- **Busy compares a hash.** The Busy rules tell paths apart by their
  length and a 32-bit hash, not their text, so two different paths of
  the same length whose hashes collide (about 1 in 4 billion per pair)
  count as one: opening the second for `Write` or removing it while the
  first is open gives `Busy`.
- **`WriteFile` uses `<name>.tmp`** and truncates that file even when
  someone else has it open.
- **LittleFS formats itself** when mounting fails with a corrupt or
  invalid file system, so a firmware downgrade to an incompatible
  LittleFS version may erase `/flash`.
- **`ringlog`.** One writer per directory; a `Read` that races a
  rotation may return more than `max` lines.
- **After an I/O error** the read position of an open file may have
  moved.
- **The SD card has no detect pin.** The card is mounted on first access
  and again after an I/O error. Without a card, `WriteFile` to `/sd`
  probes twice. On exFAT the first `Usage` scans the allocation bitmap
  (not measured on large cards).
- **`gogo/kv`** values are at most 4096 bytes; `kv.Get` returns
  `NoMemory` for a larger file written through `gogo/fs`. `kv.Put` takes
  a `string` or `Bytes` only; a `[]byte` gives `BadPath`
  (*workaround:* `bytes.FromSlice`).
- **Shrinking `/flash`** (`fs.flash.size` smaller than the file system)
  needs `gogo device format-flash`, which erases it.
- **`gogo device format-flash` has not run on a board yet.** The host
  side and the protocol are tested; the board side (the RTC flag, the
  restart, the format at mount) was built but not run, so that a
  board's `/flash` was not wiped. If the flag were lost, the command
  would only restart the board without formatting.
- **`gogo push` takes images only**, one at a time, at about 40 KB/s
  (measured on the M5StickS3: 46 KB in 1.2 s), and does not tell the
  running program. Pushing over a file the program has open replaces it
  under the program, whose handle may then fail or read the old data.
  *Workaround:* push to a new name, or move other files over HTTP or
  the SD card. A push cut off (the cable pulled) leaves its temporary
  file `.gogo-push` in the target's directory until the next push there.

## Input, audio, IMU and IR

- **No power-button events.** On the StickS3 the PM1 handles its button
  itself (click resets, double click powers off); it is not wired to the
  ESP32.
- **Battery readings.** `power.Battery.MilliVolts` is the average of the
  last 4 samples and `Percent` is linear from 3300 to 4100 mV. Samples
  outside 2500..4500 mV are discarded (the StickS3's PM1 occasionally
  returns near zero). On the Cardputer ADV charging and external power
  are unknown (-1), and without eFuse calibration the ADC is only
  roughly right. In the unix tty mode the `power` port opens but never
  reports (battery events exist only in script mode).
- **IMU.** Axes are the BMI270 chip's own, not the board's, and the
  `Orientation` values are the BMI270's numbering. Gesture thresholds
  (tap sensitivity, motion thresholds, orientation hysteresis) cannot be
  tuned. With gestures on, the sensor runs at 100 Hz at least. The first
  open after boot uploads an 8 KB configuration and blocks the scheduler
  for about 250 ms. If a gesture's follow-up register read fails, that
  poll's gestures are lost.
- **IR.** `/flash` writes hold off the RMT interrupt, so a TX frame
  longer than about 47 symbols can be garbled and a long RX frame cut
  (NEC frames are not affected). On the ESP the `ir_rx` port is polled
  every 20 ms while open (battery cost not measured). RX frames that
  arrive while all three hardware slots are full are lost without being
  counted in `Dropped`, and `Dropped` mixes lost frames with truncated
  ones. The StickS3 receiver sees occasional single ~77 us glitch pulses
  as 1-value chunks. An extended NEC address whose high byte is the
  inverse of its low byte decodes as a standard address. In the unix
  simulation, frames beyond the 4-frame queue or sent while `ir_rx` is
  closed are lost; goldens must `sleep` between `ir rx` lines.
- **StickS3 5 V rail.** While an IR port is open the Grove/Hat 5 V pins
  are outputs (EXT_5V is on): nothing may feed 5 V into them then.
- **Audio is half-duplex.** One `mic` or one `speaker` at a time; on the
  StickS3 `speaker` cannot open while `ir_rx` is open.
- **Audio details.** The first `mic` open after a power cycle records
  about 1 s of zeros (codec warm-up); later opens do not, because the
  codec's analog reference stays on until reset (its idle current is not
  measured). While a `Chunk` waits, its frame is held twice (up to 2 x
  16 KB at 500 ms and 16 kHz). `port.Handoff` of a `mic` port does not
  re-check that one `Chunk` fits beside the new owner's heap, so a
  smaller new owner may drop every chunk. The StickS3's top volume is
  capped at DAC -4 dB. The Cardputer's amplifier has no enable pin and is
  always on; its idle current is not measured.
- **Drivers stay linked when switched off.** With `GOGO_IMU`, `GOGO_IR`
  or `GOGO_AUDIO` off, the common drivers (`runtime/input/imu.c`, `ir.c`,
  `audio.c`, `runtime/stream/`) stay in the firmware (a few KB); their
  ports just refuse to open. Pins reserved for a switched-off feature
  stay reserved.

## Text screen (`screen` port, `gogo/tui`)

- **One grid size per board.** Both boards show 40 x 11 cells in
  landscape; there is no portrait mode on the StickS3 and no other font
  size. A smaller `screen.Config` uses the top-left corner.
- **Cells only.** No pixel drawing, no images. Characters outside the
  Basic Multilingual Plane are stored and shown as a box.
- **Row comparison by hash.** A frame hands the display only the rows
  whose 64-bit hash changed; two different contents of one row with the
  same hash (about 1 in 2^64) would leave that row stale until its next
  change.
- **Dones can be late.** A `port.Done` the owner has no mailbox room for
  is owed and retried every 10 ms by the port's own event source (a third
  slot, `GOGO_MAX_SOURCES` 4); with 8 owed, a further `Out` is not drawn
  and gets `Done{-1}`. Wait for a Done with a timeout. A host whose
  event-source slots are all taken retries only at the next `Out`.
- **Hidden console.** While a screen is open on a board, println output
  and crash reports go to the hidden console (and serial) and show only
  after the screen closes.
- **Refused charge.** If the owner cannot take the grid's memory, the open
  is undone after the size Chunk was sent and the LCD briefly showed a
  blank grid.
- **StickS3 navigation.** Two buttons give Enter, Down and (long press)
  Esc; there is no Up, so lists wrap around. Esc fires on B's release,
  without a cue at the threshold. `tui.Edit` needs a keyboard and edits
  printable ASCII only (`NewEdit` turns other bytes into '?').
- **Terminal mode.** In a terminal, `println` output is drawn over the
  grid (the console and the grid are kept apart only on the boards).
  `gogo run` has no `--screen` option; `build/gogovm --screen` has.

## Embedding (`libgogo`)

- **Posts racing a close are lost quietly.** A post concurrent with
  closing posts (or with the program finishing) may return 0 and still
  be discarded; it shows only in `gogo_stats.posts_dropped`. A thread
  other than the owner posts only through a `gogo_poster`, which keeps
  the handle valid; posting on a bare `gogo_vm` from another thread is
  not checked, and the VM can be freed under it. Queued posts (up to
  `GOGO_POST_MAX`, 256) are host memory outside `mem_limit` until
  delivered.
- **Host port handles have a 12-bit generation.** A `gogo_port` is a slot
  index plus a generation bumped at each close of that slot, so after 4096
  more closes of the same slot a stale handle (or a `gogo_poster_port_post` still
  queued for it) names whatever port then holds the slot, as with port
  PIDs.
- **`code.Load` refused reads as `NoMemory`.** With
  `GOGO_CONFIG_CODE_LOAD` not in `gogo_config.flags`, `code.Load` returns `NoMemory`, not
  a distinct "not allowed" error.
- **Embedded VMs sharing a host directory.** VMs that run at the same
  time must not have nested `fs_root` directories (an outer program could
  move a directory out of an inner root while the inner VM walks it), and
  the directory must not be writable by other users (they could hard-link
  their files in, which the VM may then read). See `gogo_config.fs_root`.
- **An embedded VM's `fs_quota` counts regular-file bytes only.** Empty
  files and directories cost nothing, so a program under a quota can
  still create any number of them (and use the host file system's inodes
  and directory blocks). Like `--fs-quota`, it sees only the VM's own
  changes. *Workaround:* a host that runs untrusted programs also limits
  the directory with the host's own means (a disk quota, a size-limited
  file system).
- **`libgogo.a` needs POSIX.** `gogo.h` is free of threads, but the
  library is one prelinked object that calls pthreads, `getentropy`,
  `clock_gettime` and the `*at` file calls whichever header a host
  includes, so it does not link on an RTOS without POSIX. *Planned:* a
  library build without the POSIX host and the host-directory `/flash`.
- **Internal symbols in the debug archive.** `libgogo.a` exports only
  the `gogo_*` functions, but `libgogo-debug.a` (the same objects, not
  prelinked, for debuggers on macOS) exports every internal name
  (`msg_free`, `heap_alloc`, `timer_*`, `out_*`, ...), which can clash
  with a host's own symbols. *Workaround:* link `libgogo.a` in products;
  use the debug archive only to debug.
- **Linux: some checks have not run there yet.** `make test` (the embed
  tests, `make test-fs-host`'s Linux error paths, the ELF prelink step,
  `tools/libsyms.sh` and `tools/globals.sh` on ELF), `make test-lib` and
  `make test-tls-asan` pass on Ubuntu 24.04 arm64 with gcc 13, and the
  test workflow runs them daily on Ubuntu x86-64 and macOS, where they
  have passed.
  `make test-embed-tsan` and `make test-embed-leaks` (LeakSanitizer) have
  not run on Linux. The Go binding's tests (`make test-gobind`, with
  the race detector) and `libgogoc.a`'s ELF prelink (`make test-gogoc`)
  pass on Linux arm64 (Debian, GNU ld and objcopy 2.44); on x86-64 the
  test workflow is their first run.
- **`libgogoc.a` brings the Go runtime.** The compiler library is a Go
  c-archive: it adds about 5 MB and the Go runtime's threads to a
  program, and a process can link only one Go c-archive (two Go runtimes
  do not coexist), so a C program that already links another Go library
  cannot link it. A Go program uses `embed.Compile` instead.
- **In-process compilation goes through a temporary directory.**
  `embed.Compile`, `CompileFS` and `gogoc_compile` with `GOGOC_SOURCE`
  write the sources into a directory under `$TMPDIR` for the compiler to
  read, and remove it after; they fail where no temporary directory is
  writable. *Workaround:* `embed.CompileDir` (or `gogoc_compile` on a
  path) reads a directory in place.
- **The Go binding links a prebuilt `libgogo.a`.** `gogotools/embed`'s
  VM half needs the build tag `libgogo`, cgo and `libgogo.a` from `make
  lib`, named by `CGO_LDFLAGS=-L<dir>`; `go install` of the module alone
  cannot build it. go does not notice a rebuilt `libgogo.a`: `make`
  passes the archive's checksum in a link flag to force a relink; by
  hand, use `go build -a` or the same flag.
- **A blocking hook or driver holds up the VM.** The Go binding's hooks
  and port drivers run inside `Step`; one that blocks stops the VM, and
  `Run(ctx)` sees its context only between steps. *Workaround:* do slow
  work on a goroutine and report back with `Poster.PortPost` or
  `Poster.Post`.
- **The Go binding does not check threads.** A VM's methods must not be
  called concurrently; nothing stops a program that does (the race
  detector does not see into the C library). Use a `Poster` from other
  goroutines.
- **No leak checking of the net and TLS C code.** `make test-tls-asan`
  turns LeakSanitizer off, because the C suites' fixtures and gogovm's
  exit do not free their VM, so a leak in the net stack or the TLS port
  (an mbedTLS context not freed on close, say) would go unseen; macOS's
  ASan has no leak checking at all. *Planned:* free the VM at gogovm's
  exit (under `GOGO_DEBUG`) and in the fixtures' teardown, then turn
  `detect_leaks` back on.

## ESP32-S3 boards

- **The USB serial console sometimes goes silent** a few seconds after a
  reset (esptool: `No serial data received`), while the program keeps
  running. It has been seen on both boards, with and without audio; the
  cause is unknown (desktop applications that open every new serial
  device are a suspect). *Workaround:* quit such applications; replug
  the board, or reset the USB device with libusb
  (`libusb_reset_device` on the device with vendor id 0x303a) and wait
  about 3 s.
- **The screen console is slow.** With the screen on, 1000 `println`s
  take about 2.2 times as long as without. *Workaround:* print less, or
  build with the console off (`GOGO_CONSOLE=0`).
- **An IMU open soon after boot sometimes fails.** `port.Open("imu",
  ...)` with `Gestures: 0`, within about a second of boot, returned false
  in 2 of 8 tries on the StickS3 (once on each firmware, before and after
  embed mode), with no log line; the cause is not known. *Workaround:*
  retry the open.
- **GPIO debouncing can drop a press.** On a single-edge pin, an edge in
  the same direction within `DebounceMs` after an unreported opposite
  change is dropped.
- **PSRAM costs internal RAM.** Enabling PSRAM on the StickS3 (for
  module code and TLS) takes about 5.3 KB of internal RAM from the VM.
- **The GPIO port is always built in**; it has no switch.
- **Wi-Fi can come back silently.** The firmware's `CMakeLists.txt`
  always lists `esp_wifi` and `esp_event`; with `GOGO_NET=n` they drop
  out only because nothing references them. Check the map file after
  changes.
- **Not yet run on hardware:** hot loading on the Cardputer ADV (from
  `/sd`, no PSRAM), TLS on the Cardputer ADV (memory headroom beside
  Wi-Fi unmeasured), an RSA certificate chain, a bad-certificate refusal
  by name on a board, IR NEC between the two boards, IMU gestures,
  learning a real remote, the Cardputer's all-features-off firmware, the
  full `/sd` checklist (`examples/sddemo`), and keyboard auto-repeat.

## Tooling

- **`gogo setup` on Windows** needs `-port COMn` (it cannot list serial
  ports there). It flashes the firmware and the first program, but skips
  the Wi-Fi step and does not watch the program print: `gogo` cannot use
  the serial console on Windows (nor can `gogo monitor` and `gogo config`
  on a board). Not tested on Windows hardware.
- **The release `gogovm` has no `tls` port**, and the Linux VMs are
  tested only by the release workflow (linux-arm64 under qemu). A VM that
  fails to build there is left out of the release, which says so in its
  summary. *Workaround:* build gogovm from source (`make vm`).
- **`#!/usr/bin/env -S gogo run` needs an `env` with `-S`** (macOS, GNU
  coreutils 8.30+), not BusyBox's. *Workaround:* `#!/usr/local/bin/gogo
  run`, the absolute path of gogo.
- **`gogo setup` trusts the release it is pointed at.** `SHA256SUMS` comes
  from the same release as the files, so it catches broken downloads,
  not a tampered release; there are no signatures.

- **`gogo config -local` stores secrets in plain text.** `set
  wifi.password -` (or `node.token`, or an `import` holding them) is
  accepted for the local file (`~/.gogo/config` or `$GOGO_CONFIG`, mode
  0600) with a warning, although programs never see `wifi.` keys and the
  host has no Wi-Fi.
- **`gogo config export` cannot copy secrets**: the board never sends the
  Wi-Fi password back, so `wifi.password` and `node.token` are written
  as comments to fill in by hand.
- **`gogo config`'s password prompt** exits with 130 on SIGTERM and
  SIGHUP too (convention: 143 and 129).
- **The tty key mode** (`gogo run --key`) reads the terminal only when
  the VM is idle, so a busy actor delays key events.
- **Test-only VM flags** (`--without`, `--fs-quota`, `--net-fd`,
  `--sim-clock`) are not passed through by `gogo run`. *Workaround:* run
  `build/gogovm` directly on an image built with `gogo build`.
- **`--fs-quota` is per medium** (`/flash` and `/sd` each), counts the
  files already in the directory, and gives room back on delete and
  truncation; it only sees the VM's own changes, so files the host adds
  or removes meanwhile make it approximate.
- **Host storage does not follow symlinks.** With `--fs-root` (and an
  embedded VM's `fs_root`), a symlink inside the directory is `BadPath`
  to open or walk through and `ReadDir` lists it as a file; FIFOs and
  devices are `BadPath` too.
- **`tools/vmsize.sh --vfs`** reports a smaller fixed state than the
  boards have (it builds with the default slot size); the compile-time
  assertions in the board's file system glue are the real check.
