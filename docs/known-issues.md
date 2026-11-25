# Known issues and limitations

Open limitations of gogo as of image version 13, by area. Each entry
says what the limitation is, what it costs you and, where there is one,
how to work around it. Hardware facts refer to the two ESP32-S3 boards,
the M5StickS3 and the Cardputer ADV. The design is in
[design.md](design.md); the book's appendix lists the documented limits
([13-appendices.md](book/13-appendices.md)).

## VM

- **A crash does not change the exit status.** `gogovm` (and so
  `gogo run`) exits 0 when the run queue empties, even after the main
  actor crashed (`gogo: actor <pid 0> crashed: divzero in main`). A
  program cannot set an exit status either: `actor.Exit` only ends the
  actor. *Workaround:* scripts and CI must look for `crashed:` on stderr.
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
  handle fail (a TCP write ends with `Closed{Overflow}`). *Workaround:*
  do not keep many large frozen `Bytes` alive in one program.
- **Host and board memory limits differ by 16x.** An actor spawned
  without `SpawnOpt` may use 256 KB on the host and 16 KB on a board, so
  a program that passes under `gogo run` can die with `oom` on the board.
  *Workaround:* test with `build/gogovm_mcu` (built by `make test-go`),
  which has the board limits: `GOGO_VM=build/gogovm_mcu gogo run ...`.
- **`gogo/supervisor` supervises one child.** There is no multi-child
  `one_for_one` strategy. *Workaround:* one supervisor per child.
- **Small costs on hot paths.** Sending to a dead or stale PID walks the
  message once (to check it). Every port command and `port.Open`
  validates its whole value without a visited set, so a large shared
  structure is walked once per path to it.

### Hot loading

- **No signatures and no permissions.** Any actor that can hand
  `code.Load` some bytes can run code with the same rights as the main
  program. The `requires` bits say what the firmware must provide, not
  what a module may do. Function, type and jump indexes are checked when
  used (`badcode`), not when loaded. Load only images you trust.
- **Modules live in RAM only.** There is no automatic loading from flash
  at boot and no execute-in-place from flash. *Workaround:* store the
  module with `fs.WriteFile` and load it with `fs.ReadFile` +
  `code.Load` at start.
- **No `sys.Has`.** A program cannot ask which capabilities the firmware
  has before loading; it learns them from `code.Missing` after
  `Unsupported`.
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
  (`uint32` included), floats, package-level `var`s, the expression
  `switch`, type assertions, `panic`, the `error` type, assignment to
  struct fields, labeled statements, interfaces other than `any`,
  variadic functions and packages of your own (only `package main`,
  possibly in several files, plus the `gogo/...` libraries). Channels,
  `go`, `select`, generics, `defer` and `recover` are outside the design.
  "Language completeness" is a planned milestone. *Workarounds:* `if`/
  `else if` chains for `switch`, a type switch for assertions, slices of
  structs searched linearly for maps, rebuilding a struct with a literal
  instead of assigning a field.
- **No `gogo/fmt` or `gogo/errors`.** Printing is `println`; to build a
  string use `gogo/strconv` and `+`.
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
- **Capability inference is a heuristic.** A constant `"/sd"` path
  passed to `gogo/fs` makes the image require `sd`, even if the program
  only probes for a card; a driver name known only at run time gives a
  build warning and is not checked at load. *Workaround:* pass the path
  through a variable (as `examples/audiohw` does).
- **Some error messages are unhelpful.** An interface type gives `type
  definition other than struct or int, int64, bool, string not
  supported`; a variadic parameter gives `Ellipsis not supported`.
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
  connection takes one `Write` in flight and one queued. A third `Write`
  before a `Written` arrives (or writing on while three `Written` are
  owed) ends the connection with `Closed{Overflow}` and a reset. A naive
  echo server works in light testing and breaks under load.
  *Workaround:* count `Written` events and buffer
  (`docs/book/examples/10-tcp-echo` does).
- **Small writes need a `Bytes` handle.** A small `Write` is copied into
  a port-held handle (for retransmission); if the VM's 64 handles are all
  taken, the connection ends with `Closed{Overflow}`.
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
  with exit code 100.
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
  needs `fs.flash.format=yes`, which erases it.
- **No file transfer** between a computer and `/flash`. *Workaround:*
  move files over HTTP, or use the SD card.

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
- **Audio is half-duplex.** One `mic` or one `spk` at a time; on the
  StickS3 `spk` cannot open while `ir_rx` is open.
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
- **GPIO debouncing can drop a press.** On a single-edge pin, an edge in
  the same direction within `DebounceMs` after an unreported opposite
  change is dropped.
- **PSRAM costs internal RAM.** Enabling PSRAM on the StickS3 (for
  module code and TLS) takes about 5.3 KB of internal RAM from the VM.
- **The GPIO port is always built in**; it has no switch and no
  capability bit.
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

- **`gogo config -local` stores secrets in plain text.** `set
  wifi.password -` is accepted for the local file (`~/.gogo/config` or
  `$GOGO_CONFIG`), although programs never see `wifi.` keys and the host
  has no Wi-Fi.
- **`gogo config`'s password prompt** exits with 130 on SIGTERM and
  SIGHUP too (convention: 143 and 129).
- **The tty key mode** (`gogo run --key`) reads the terminal only when
  the VM is idle, so a busy actor delays key events.
- **Test-only VM flags** (`--caps`, `--fs-quota`, `--net-fd`,
  `--sim-clock`) are not passed through by `gogo run`. *Workaround:* run
  `build/gogovm` directly on an image built with `gogo build`.
- **`tools/vmsize.sh --vfs`** reports a smaller fixed state than the
  boards have (it builds with the default slot size); the compile-time
  assertions in the board's file system glue are the real check.
