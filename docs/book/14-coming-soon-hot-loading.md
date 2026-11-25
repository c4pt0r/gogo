# 14. Coming Soon: Hot Loading

> **Not yet available.** Everything in this chapter describes a design
> that has been approved (`docs/superpowers/specs/2026-11-24-gogo-hotload-design.md`)
> and is being built on a separate branch. The `gogo/code` package does
> not exist in the current gogo, none of the code below compiles today,
> and details may still change. There are no runnable examples for this
> chapter.

## The idea

Today a board runs the one program you flashed. Hot loading lets a running
program **load more code while it runs**: an actor receives a compiled
module (over the network, from a file, or in a message), asks the VM to
load it, looks up an entry function by name, and starts it in a **new
actor**.

Running code is never changed in place. To upgrade, the old actor exits
and its supervisor restarts it with the newly loaded code. This keeps the
actor model intact: the unit of replacement is an actor, and replacement
is just a supervised restart.

## The planned API

A new stub package, `gogo/code`:

```go
package code

type Module struct{ /* opaque handle */ }
type Err int // OK, BadImage, Unsupported, NoMemory, NotFound, BadArg

func Load(b bytes.Bytes) (Module, Err)
func Spawn(m Module, entry string, arg any) (actor.PID, Err)
func SpawnLink(m Module, entry string, arg any) (actor.PID, Err)
func SpawnOpt(m Module, entry string, arg any, opts actor.Options) (actor.PID, Err)
func Missing(m Module) int // after Unsupported: the features the firmware lacks
```

- **Entry functions** are exported functions of the module's `package
  main` with the signature `func(arg any)` or `func()`, looked up by
  name (case-sensitive). `arg` is copied into the new actor like a
  message.
- A **`Module` handle** can be sent to other actors like a PID, and they
  can spawn from it too. Modules can load other modules.
- **Every image is a module.** The main program and loaded modules use the
  same format and are built the same way (`gogo build ./sensor -o
  sensor.gogo`). At boot the firmware loads the flashed image as module 0
  and spawns its `main`.

## Safety

- `code.Load` checks everything before accepting a module: format and
  version, all bounds, the constant area, the export table, and the
  features the module needs against the features the firmware was built
  with (`Unsupported`, with `Missing` saying what is absent). A bad or
  malicious module makes `Load` return an error; the host is unaffected.
- Every reference into a module's code carries the module's slot and
  generation, and is checked on use, so a stale reference fails with
  `badcode` instead of reading freed memory.
- **Messages between modules** may carry only basic values, `Bytes`, PIDs,
  Refs, `Module` handles and the gogo packages' own struct types, never a
  module's own structs or closures. A sender that breaks this rule exits
  with a new reason, `BadMsg`, whichever module it belongs to; its
  supervisor handles that like any crash. `code.Spawn` with such an `arg`
  returns `BadArg` instead.
- This first version gives modules the same rights as the main program
  (ports, files, network, configuration). Signing and checking modules is
  planned together with HTTPS.

## Memory

- Module code runs from RAM: on the Cardputer ADV directly from the
  received `Bytes`, on the M5StickS3 from a copy in its PSRAM. Nothing is
  written to flash, so loading is fast and does not stall the device, and
  a loaded module is gone after a reboot (store it as a file and load it
  at boot if you need it again).
- All loaded module code together is limited by the configuration key
  `code.mem` (default 64 KB); over it, `Load` returns `NoMemory`. Code
  memory is not charged to any actor.
- A module stays loaded while an actor running its code is alive, or while
  some actor **holds** it (`code.Load` holds it for the caller; others can
  `code.Hold(m)`, and `code.Release(m)` or the holder's exit ends a hold).
  When neither is true, it is unloaded and its handle becomes stale.

## Supervised upgrades

`gogo/supervisor` will gain a supervisor whose child is a module entry:

```go
sup := supervisor.SuperviseModule(m, "Main", arg, spec)
```

It holds the module and restarts the child with `code.SpawnLink(m, "Main",
arg)` after a crash. To upgrade, send it `supervisor.Upgrade{Module: m2}`:
it holds the new module, stops the old child, starts the new one, and
releases the old module, which unloads once its last actor is gone.

## Not planned in this version

Writing modules to flash or replacing the whole firmware image, changing
code inside a running actor (Erlang-style code upgrade), and calling
functions or sharing types across modules (modules talk only by
messages).
