# 14. Hot Loading

Until now a board has run the one program you flashed. **Hot loading** lets
a running program load more code while it runs: an actor gets a compiled
module (from a file, from the network, or in a message), asks the VM to
load it, and starts one of its entry functions in a **new actor**.

Running code is never changed in place. To upgrade, the old actor exits
and its supervisor restarts it with the newly loaded code. The unit of
replacement is an actor, and replacement is just a supervised restart.

The package is `gogo/code`. Its full reference is in
[`docs/design.md`](../design.md) section 5.4.4; its open limitations are in
[`docs/known-issues.md`](../known-issues.md).

## 14.1 Modules

**Every image is a module.** The main program and a loaded module use the
same format and are built the same way:

```sh
gogo build ./greeter -o greeter.gmod     # the extension is only a habit
```

A module is an ordinary gogo `package main`. Its **entries** are the
exported functions with the signature `func()` or `func(arg any)`, found by
name (case-sensitive). `func main()` is exported as the entry `main`. At
boot the firmware loads the flashed image as module 0 and starts its
`main` the same way `code.Spawn` would.

```go
// mods/greeter/main.go
package main

import "gogo/actor"

// Greet is an entry: it tells the PID it was started with who it is.
func Greet(arg any) {
	switch host := arg.(type) {
	case actor.PID:
		actor.Send(host, "hello from a module")
	}
}

func main() {}
```

## 14.2 Loading and starting

```go
func Load(b bytes.Bytes) (Module, Err)
func Spawn(m Module, entry string, arg any) (actor.PID, Err)
func SpawnLink(m Module, entry string, arg any) (actor.PID, Err)
func SpawnOpt(m Module, entry string, arg any, opts actor.Options) (actor.PID, Err)
func Hold(m Module) Err
func Release(m Module)
func Missing(m Module) int   // after Unsupported: the features the firmware lacks
func MemUsed() int           // bytes of module code in use (counted against code.mem)
```

`Err` is `OK` (0), `BadImage`, `Unsupported`, `NoMemory`, `NotFound`,
`BadArg` or `TooMany`. `arg` is copied into the new actor like a message.
A `Module` is a plain value you can send to other actors, which can spawn
from it too.

The example `examples/14-load` reads a module from `/flash`, loads it,
starts `Greet`, and then shows the errors and the unload:

```go
b, e := fs.ReadFile("/flash/greeter.gmod", 8192)
m, err := code.Load(b)
println("load:", int(err), "code mem in use:", code.MemUsed() > 0)

p, err := code.Spawn(m, "Greet", actor.Self())
...
```

Its output (`run.sh` builds `mods/greeter` into `/flash/greeter.gmod` before
running it):

```text
load: 0 code mem in use: true
spawn Greet: 0 true
hello from a module
spawn greet: 4 NotFound is 4
code mem in use after release: 0
spawn after unload: 4
```

A wrong entry name (`greet` is not `Greet`) is `NotFound`, not a crash. A
bad or malicious module makes `Load` return an error; the host is
unaffected.

## 14.3 Lifetime

A module stays loaded while an actor running its code is alive, or while
some actor **holds** it. `code.Load` holds it for the caller; other actors
can `code.Hold(m)` (at most four holders). `code.Release(m)` or the
holder's exit ends a hold. When there is neither a live actor nor a hold,
the module is unloaded: its code memory is freed and its handle is
**stale**. `Spawn` with a stale handle returns `NotFound`, also after a new
module took the same slot, and a value that still points into the unloaded
module (a constant string, a function value, a struct of its own type) makes
the actor that uses it exit with `BadCode`.

The VM holds at most 8 modules, the main program included; a ninth `Load`
is `TooMany`.

## 14.4 Supervised upgrades

`gogo/supervisor` has a supervisor whose child is a module entry:

```go
sup := supervisor.SuperviseModule(m, "Work", arg, spec)
```

It holds the module and restarts the child with `code.SpawnLink(m, "Work",
arg)` after a crash. If it cannot start (not loaded, no such entry) it
returns the zero PID. To upgrade, send it `supervisor.Upgrade{Module: m2}`
with `actor.Call`; the reply is an `int`, a `code.Err`: the supervisor holds
the new module, stops the old child, starts the new one with the same entry
and argument, and releases the old module, which unloads once its last
actor is gone. If anything fails, the old module keeps running and the
reply says why. Messages sent to the old child between its stop and the
new start are lost.

The example `examples/14-upgrade`: `worker_v1` crashes on its third
message, the supervisor restarts it from the module, then the host loads
`worker_v2` and upgrades without stopping.

```go
v1 := load("/flash/worker_v1.gmod")
sup := supervisor.SuperviseModule(v1, "Work", actor.Self(),
	supervisor.Spec{Restart: supervisor.Permanent, MaxRestarts: 3})
...
v2 := load("/flash/worker_v2.gmod")
code.Release(v1) // the supervisor's hold keeps v1 until the upgrade
r, _ := actor.Call(sup, supervisor.Upgrade{Module: v2}, 2000)
println("upgrade result (0 is ok):", r)
```

```text
loaded v1: true
worker started
worker v1: message 1
worker v1: message 2
worker v1: crashing
worker restarted from the module
worker v1: message 1
loaded v2, both resident: true
upgrade result (0 is ok): 0
upgraded to v2
v1 unloaded, only v2 resident: true
worker v2: message 1
worker v2: message 2
```

## 14.5 Messages between modules

Code never runs across modules, and modules do not share types. When the
sender and the receiver belong to different modules (an actor belongs to
the module it was spawned from, and so do the actors it spawns), a message
may carry only:

- basic values, strings and `Bytes` (constants are copied to the heap, so
  the receiver never points into the sender's code),
- PIDs, Refs and `Module` handles,
- the struct types of gogo's own packages (`actor.Request`, `net.Up`, ...).

A module's **own** structs and function values may not cross. The sender
that tries exits with the reason `BadMsg` (whichever module it belongs
to; the main program has no special case), and its supervisor handles that
like any crash. `code.Spawn` with such an `arg` returns `BadArg` instead of
crashing. Messages inside one module are unaffected.

## 14.6 What a module may do, and what the firmware offers

- **Capabilities.** The compiler records which optional features an image
  needs (the network packages, the `kbd`, `power`, `imu`, `ir_tx`, `ir_rx`,
  `mic`, `spk` and `tls` ports, `/sd`). Drivers that a library marks
  `//gogo:optional` do not count for that library's own opens (chapter
  12). Each firmware knows which it was built with. `code.Load` of a module that needs more returns `Unsupported`, and
  `code.Missing(m)` gives the missing bits (`code.CapNet`, `CapKbd`,
  `CapPower`, `CapSD`, `CapIMU`, `CapIRTX`, `CapIRRX`, `CapMic`, `CapSpk`,
  `CapTLS`;
  appendix F). The main image is checked the same way at boot.
- **Rights.** This version has no permissions: a module can do everything
  the main program can (ports, files, network, configuration). Signing and
  permissions are not built yet; they were meant to come with HTTPS, which
  does not add them (chapter 17).
- **Memory.** All loaded module code together is limited by the
  configuration key `code.mem` (default 64 KB):

  ```sh
  gogo config set code.mem 128K
  ```

  Over it, `Load` returns `NoMemory`. Code memory is not charged to any
  actor, and the main program does not count. Module code runs from RAM:
  from the received `Bytes` directly when they are a whole off-heap `Bytes`
  at a 4-byte aligned address, otherwise from a copy. On the M5StickS3 the
  copy goes into its 8 MB of PSRAM, so module code does not use up the small
  internal RAM; the Cardputer ADV has no PSRAM and keeps the code in RAM.
  Nothing is written to flash, so loading is fast, and a loaded module is
  gone after a reboot: store it as a file and load it at boot if you need it
  again.

## 14.7 Fetching a module over the network

`examples/hotload_net` in the repository does it on a board: it joins
Wi-Fi, downloads `worker_v1.gmod` with `http.Do` from the base URL in the
config key `demo.modurl`, loads it, runs it under `SuperviseModule`, waits,
downloads `worker_v2.gmod` and upgrades. The body of an HTTP response is
charged to the actor that asked, whose default limit on a board is 16 KB,
so the example limits the module to 4 KB (`modBody`, passed as `maxBody`); for bigger modules
raise it, up to `http.MaxBodyLimit`, in an actor started with a larger
`MaxMemory`. Serve the modules from your computer with
`python3 -m http.server`; the example's README has the exact commands.

(This example cannot run on the host, which has no network, so it is not
among the checked examples.)

## 14.8 Not in this version

Writing modules to flash or replacing the whole firmware image, changing
code inside a running actor (Erlang-style code upgrade), calling functions
or sharing types across modules, signing, and per-module permissions.
