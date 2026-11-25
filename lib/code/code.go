// Package code is the gogo hot-loading API: load a module (an image built
// by `gogo build`, as bytes.Bytes from a file, the network or a message)
// and start its entry functions in new actors. It is a stub package like
// gogo/fs: the compiler replaces every call with the VM builtin B_CODE, so
// the bodies are never executed. The C side is runtime/vm/module.c.
//
// Every image is a module. Its entries are the exported functions of its
// package main with signature func() or func(arg any), found by name (case
// matters), and func main(), exported as "main".
//
// A module stays loaded while an actor runs its code or an actor holds it.
// Load makes the caller a holder; Hold adds another actor (at most 4
// holders per module), Release or the holder's exit ends a hold. With
// neither left the module unloads: its code memory is freed and every
// Module naming it is stale (Spawn and Hold give NotFound, also after a
// new module took its place). Values that still refer into an unloaded
// module (its constant strings, its struct types, its functions) make the
// actor that uses them exit with BadCode.
//
// Memory. The code of all loaded modules together may use at most the
// config key code.mem bytes (`gogo config set code.mem 64K`; a K or M
// suffix; default 64K); a Load over it is NoMemory. The main program (the
// boot image) is not counted. A module runs from its Bytes in place when
// that is a whole off-heap Bytes (not a Slice) at a 4-byte aligned address
// (the module keeps the Bytes); other Bytes (heap, any Slice) are copied
// once, and on boards with PSRAM the code is always copied there. The count
// is the image size either way, which is all the module keeps.
//
// There are no permissions in this version: a module can do everything the
// main program can. Arguments are copied into the new actor like a message.
//
// The struct type Module has the fixed type index 34 in every image
// (TYPE_CODE_MODULE in runtime/vm/opcodes.h); keep its fields in sync with
// compiler.reservedTypes.
package code

import (
	"gogo/actor"
	"gogo/bytes"
)

// Module is a loaded module, or none (the zero Module). It is a plain value:
// send it to another actor, which may Spawn from it or Hold it. Having a
// Module does not keep the module loaded; only holds and running actors do.
// H names the module (0: none); Missing is what Missing returns. Both are
// plain ints, not secrets: a Module built by hand names a module only if
// its H is exactly what Load returned for one still loaded, and never the
// main program; any other value is simply NotFound.
type Module struct{ H, Missing int }

// Err is the result of every call; OK is 0.
type Err int

// Error values (same numbers as CODE_* in runtime/vm/module.h).
const (
	OK          Err = 0
	BadImage    Err = 1 // not exactly one valid image of this VM's format
	Unsupported Err = 2 // needs a firmware capability it lacks: see Missing
	NoMemory    Err = 3 // over code.mem, or out of memory
	NotFound    Err = 4 // no such module (unloaded, or the zero Module) or entry
	BadArg      Err = 5 // an argument that cannot be passed
	TooMany     Err = 6 // the module table (8, the main program included) or the holders (4) are full
)

// Capability bits (Missing; CAP_* in runtime/vm/platform.h).
const (
	CapNet     = 1       // gogo/net, dhcp, dns, http
	CapKbd     = 2       // the "kbd" port
	CapPower   = 4       // the "power" port
	CapSD      = 8       // the /sd file system
	CapIMU     = 16      // the "imu" port
	CapIRTX    = 32      // the "ir_tx" port
	CapIRRX    = 64      // the "ir_rx" port
	CapMic     = 128     // the "mic" port
	CapSpk     = 256     // the "spk" port
	CapTLS     = 512     // the "tls" port
	CapAnyPort = 1 << 31 // only a build-time marker: never missing
)

// Load checks b and loads it as a module held by the caller. The checks, in
// order: BadImage, Unsupported, NoMemory, TooMany. On error the Module is
// none; after Unsupported, Missing tells what the firmware lacks.
func Load(b bytes.Bytes) (Module, Err) { panic("gogo only") }

// Spawn starts entry of m in a new actor running m's code, with arg (any
// value; an entry without a parameter ignores it). NotFound if m is not
// loaded or has no such entry, BadImage if the entry's function is
// malformed (more parameters than registers; never from `gogo build`).
func Spawn(m Module, entry string, arg any) (actor.PID, Err) { panic("gogo only") }

// SpawnLink is Spawn, linked to the caller (actor.SpawnLink).
func SpawnLink(m Module, entry string, arg any) (actor.PID, Err) { panic("gogo only") }

// SpawnOpt is Spawn with options (actor.SpawnOpt).
func SpawnOpt(m Module, entry string, arg any, opts actor.Options) (actor.PID, Err) {
	panic("gogo only")
}

// Hold makes the caller a holder of m: m stays loaded until the caller
// Releases it or exits. Holding twice is one hold. NotFound if m is not
// loaded, TooMany if it has 4 holders.
func Hold(m Module) Err { panic("gogo only") }

// Release ends the caller's hold on m, if any; m unloads if nothing else
// keeps it.
func Release(m Module) { panic("gogo only") }

// Missing is the capability bits (Cap*) the firmware lacked when Load
// returned m with Unsupported; 0 for any other Module from Load (it is
// m.Missing).
func Missing(m Module) int { panic("gogo only") }

// MemUsed is the code memory (bytes) of the loaded modules, counted against
// code.mem.
func MemUsed() int { panic("gogo only") }
