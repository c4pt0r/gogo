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
  its flags before the program only. A usage
  mistake prints what is wrong before the usage.
- **Configuration keys.** `gogo config set` takes keys of `a-z 0-9 . _`
  only (upper case and `-` were allowed), refuses unknown keys under the
  gogo prefixes `wifi.`, `net.`, `fs.`, `code.` and `gogo.`, and checks
  the values of gogo's keys (book chapter 11). `get` and `rm` still take
  the old spelling.
- **Exit statuses** of `gogo run` and `gogovm` (book chapter 12): a
  crash of `main` exits 4 and `actor.Exit(code)` in `main` exits with
  the code (0 before). A missing driver or mount exits 3 (was 1), a bad
  image still 2, and a `gogovm` usage error 1 (was 2).

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
  the whole timeout. *Workaround:* monitor the server and keep timeouts
  short; treat `ok == false` as "maybe dead".
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
- **The `bytes.Bytes` handle table is small.** The whole VM has 64
  off-heap handles (`GOGO_BYTES_HANDLES`) and one actor may hold 16. When
  the table is full, the VM first collects the holders; if that frees
  nothing, the allocation fails (`OutOfMemory`) and ports that need a
  handle refuse (a stream `Out` gets `Done{-1}`). *Workaround:*
  do not keep many large frozen `Bytes` alive in one program; on a
  board, `gogo device stats` shows the table's use (`bytes=N/64`, with
  `ports=N/16` and `subs=N/4` for the port and `net.Subscribe` tables).
- **Host and board memory limits differ by 16x.** An actor spawned
  without `SpawnOpt` may use 256 KB on the host and 16 KB on a board, so
  a program that passes under `gogo run` can die with `oom` on the board.
  *Workaround:* test with `build/gogovm_mcu` (built by `make test-go`),
  which has the board limits: `GOGO_VM=build/gogovm_mcu gogo run ...`.
- **`gogo/supervisor` supervises one child.** There is no multi-child
  `one_for_one` strategy. *Workaround:* one supervisor per child.
- **Small costs on hot paths.** Sending to a dead or stale PID walks the
  message once (to check it).
- **Values with heavily shared parts.** `println`, every port command and
  `port.Open` walk the whole value (to depth 16) without a visited set,
  so a part reachable by many paths is walked once per path. The walk
  stops after 65536 terms, so it takes at most a fraction of a
  millisecond on the host: `println` then ends the value with ` ...`,
  and a port command or `port.Open` ends the actor with `badcode`. A
  value without shared parts never reaches the limit (an actor's heap
  holds fewer words), but moderate sharing in a large value can: 5000
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
  `port.Open`'s `ok=false`.
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

- **Not supported yet (planned).** The compiler rejects maps, methods,
  pointers, arrays, integer types other than `int`, `int64` and `byte`
  (`uint32` included), floats, package-level `var`s, a `switch` on an
  `any` or a struct value, type assertions, `panic`, the `error` type,
  assignment to struct fields and interfaces other than `any`. `goto` is rejected (labeled `break` and
  `continue` cover its uses). Channels, `go`, `select`, generics, `defer`
  and `recover` are outside the design. "Language completeness" is a
  planned milestone. *Workarounds:* a type switch for assertions and for
  values in an `any`, slices of structs searched linearly for maps,
  rebuilding a struct with a literal instead of assigning a field.
- **No `gogo/fmt` or `gogo/errors`.** Printing is `println`; to build a
  string use `gogo/strconv` and `+`.
- **A shared package's struct types cannot cross modules.** A program
  and a hot-loaded module (or two modules) that import the same package,
  your own or a `gogo/` library such as `gogo/http`, each compile their own
  copy of it, so its struct types are different types in each. Sending
  one across modules fails with `BadMsg` at run time, not at compile time
  (book 14.6). Only the stub packages' reserved types (`actor.Request`,
  `net.Up`, `fs.Info`, ...) cross. *Workaround:* send basic values,
  strings or `Bytes`, and rebuild the struct in the receiver.
- **Scripts get no arguments yet.** `gogo run x.gg args...` and `./script
  args...` are refused until programs can read their arguments.
- **Loop variables cannot be captured.** In a three-clause `for` loop,
  `func() { work(i) }` is rejected with `captured variable must not be
  reassigned`, because `i++` assigns `i`. Closures capture by value, so
  every captured variable must be assigned once. *Workaround:* copy
  first: `n := i`.
- **Two-result builtins cannot stand alone.** `actor.Call(p, req, 100)`
  or `actor.ReceiveTimeout(0)` as a statement fails with `multiple values
  not supported`. *Workaround:* `_, _ = actor.Call(...)`. `println(f())`
  with a multi-result `f` and `var a, b = f()` fail the same way; use
  `a, b := f()`.
- **`actor.PID{}` is rejected.** *Workaround:* `var none actor.PID` is the
  zero PID.
- **Some values cannot go into `any`.** `byte`, `rune`, named basic types
  (`type C int`) and func values are rejected, because at run time they
  are indistinguishable from `int` (or have no type id). Func values
  cannot be compared, not even with `nil`. `type B byte` is rejected.
  *Workaround:* convert to `int` first; wrap a func value in a struct.
- **`fs.File` and `actor.PID` share a type tag.** `fs.File` cannot be a
  type switch case, and an `fs.File` in an `any` is seen as a PID.
  *Workaround:* wrap it in a struct.
- **No dead-code elimination.** Importing a library compiles all of it
  into the image (`gogo/strings` costs about 440 words, `gogo/strconv`
  about 360), which matters on boards with little flash.
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
- **Some error messages are unhelpful.** An interface type gives `type
  definition other than struct or int, int64, bool, string not
  supported`.
- **`println` of a slice differs from Go.** gogo prints the elements,
  `[1 2 3]`; Go's `println` prints `[len/cap]address` (only
  `fmt.Println` prints elements).
- **Library differences from Go.** `strconv` handles bases 2 to 36 only
  (no prefixes, no underscores) and `Atoi` overflows at 32 bits;
  `strings.TrimSpace`, `ToLower` and `EqualFold` are ASCII-only and
  `Split(s, "")` splits into bytes; slices grow by doubling from a
  capacity of 4.

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
  a port-held handle (for retransmission); if the VM's 64 handles are all
  taken, it is refused with `Done{-1}`.
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
  with exit code 69, `http.ListenFailed` (`actor.Failed + errors.Busy`),
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

## Storage

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
  slot, `GOGO_MAX_SOURCES` 3); with 8 owed, a further `Out` is not drawn
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
  not run on Linux.
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

- **Release downloads need a GitHub login while the repository is
  private.** `gogo setup` downloads anonymously, so it gets a 404 from
  `github.com/c4pt0r/gogo/releases` until the repository is public.
  *Workaround:* `gh release download <tag> -R c4pt0r/gogo -D gogo-release`
  (with an authorized `gh`), then `gogo setup -release-url gogo-release`;
  or build the firmware locally with `gogo setup -offline` inside the
  source tree.
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
  wifi.password -` is accepted for the local file (`~/.gogo/config` or
  `$GOGO_CONFIG`), although programs never see `wifi.` keys and the host
  has no Wi-Fi.
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
