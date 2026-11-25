// Package port is the gogo peripheral API: a port is a special actor with a
// PID, backed by a platform driver. These stubs exist only so programs
// type-check with the standard Go toolchain; the gogo compiler replaces
// every call with VM instructions, so the bodies are never executed.
//
// The struct types GPIO, Write and Edge have fixed type indices 6-8 in every
// image, Chunk, Out and Done 35-37, Host and Post 45-46 (TYPE_* in
// runtime/vm/opcodes.h); keep their field order in sync with
// compiler.reservedTypes.
//
// Send a command to a port with actor.Send(p, port.Write{Value: 1}); any
// actor holding the PID may. Events (port.Edge) go to the owner, the actor
// that opened the port, as ordinary messages under its mailbox quota. The
// owner's exit closes its ports; actor.Kill on a port PID closes it. Ports
// cannot be linked or monitored. The owner may give a port to another actor
// with Handoff.
package port

import (
	"gogo/actor"
	"gogo/bytes"
)

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

// Chunk is a stream event, the one message type of every port that delivers
// data in blocks (the "imu" sample batches, "ir_rx" frames). Seq counts the
// chunks the port made, from 1, dropped ones included, so a gap in Seq means
// chunks were lost; TimeMs is when the data began (the low 31 bits of the
// millisecond clock; each port documents its precision); Dropped is how
// many chunks were dropped since the last one delivered (a chunk over the
// owner's mailbox quota is dropped and counted; the owner is not killed).
type Chunk struct {
	Seq, TimeMs, Dropped int
	Data                 bytes.Bytes
}

// Out is a stream command: send Data to a port that outputs a stream (the
// "ir_tx" timings). At most 2 Outs are in flight per port (one running, one
// queued).
type Out struct{ Data bytes.Bytes }

// Done is a stream event: an Out has finished. Every accepted Out gets one,
// in the order the Outs were sent; a refusal (Seq -1) is dropped only when
// 16 Dones are already owed and unread (an owner that reads nothing, flooded
// with Outs). Seq numbers the Outs sent successfully, from 1; it is -1 when the port refused the Out (too many in flight, or bad
// data) or the hardware could not send it, and such an Out uses no number.
type Done struct{ Seq int }

// Host configures a port that the program embedding the VM defines (embed
// mode, gogo_register_port in runtime/include/gogo.h): Arg and Data are
// passed to the host's open as they are. Such a port takes Post commands
// and sends its owner Post events.
type Host struct {
	Arg  int
	Data bytes.Bytes
}

// Post is a message from the program embedding the VM (embed mode). The
// host's gogo_post sends one to a registered name: Kind and Data are what
// the host gave; it arrives as an ordinary message under the receiver's
// mailbox quota (a post over it is dropped, and the receiver is not
// killed). Host-defined ports use it for their commands and events too.
type Post struct {
	Kind int
	Data bytes.Bytes
}

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
