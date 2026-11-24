// Package port is the gogo peripheral API: a port is a special actor with a
// PID, backed by a platform driver. These stubs exist only so programs
// type-check with the standard Go toolchain; the gogo compiler replaces
// every call with VM instructions, so the bodies are never executed.
//
// The struct types GPIO, Write and Edge have fixed type indices 6-8 in every
// image (TYPE_* in vm/opcodes.h); keep their field order in sync with
// compiler.reservedTypes.
//
// Send a command to a port with actor.Send(p, port.Write{Value: 1}); any
// actor holding the PID may. Events (port.Edge) go to the owner, the actor
// that opened the port, as ordinary messages under its mailbox quota. The
// owner's exit closes its ports; actor.Kill on a port PID closes it. Ports
// cannot be linked or monitored. The owner may give a port to another actor
// with Handoff.
package port

import "gogo/actor"

// GPIO configures a "gpio" port: one pin.
type GPIO struct {
	Pin        int
	Mode       int // Input, InputPullUp, InputPullDown, Output
	Edge       int // None, Rising, Falling, Both (inputs only)
	DebounceMs int // input debounce; <= 0 means the default, 20 ms
}

// Write is a command: drive an output pin to Value (0 or 1).
type Write struct{ Value int }

// Edge is an event: the pin's level changed; Value is the level after it.
type Edge struct{ Pin, Value int }

// GPIO modes.
const (
	Input         = 0
	InputPullUp   = 1
	InputPullDown = 2
	Output        = 3
)

// GPIO edges.
const (
	None    = 0
	Rising  = 1
	Falling = 2
	Both    = 3
)

// Open opens a port of the named driver with cfg; the caller becomes its
// owner. ok is false if the driver is unknown, cfg is invalid for it, the
// resource is taken, or memory runs out.
func Open(driver string, cfg any) (p actor.PID, ok bool) { panic("gogo only") }

// Close closes the port p. A stale or unknown p is ignored.
func Close(p actor.PID) { panic("gogo only") }

// Handoff makes the actor to the owner of port p: its later events go to
// to, to's exit closes it, and the memory charged for it (driver state,
// buffers the port holds) moves to to. Only p's owner may hand it off. ok
// is false (nothing changes) if the caller does not own p, p is not an open
// port, to is not a live actor, or to cannot take the charge within its
// memory limit. Handoff to the caller itself only resumes a paused port
// (e.g. an accepted net connection).
func Handoff(p actor.PID, to actor.PID) (ok bool) { panic("gogo only") }
