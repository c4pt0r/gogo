// Package actor is the gogo actor API. These stubs exist only so programs
// type-check with the standard Go toolchain; the gogo compiler replaces every
// call with VM instructions, so the bodies are never executed.
package actor

// PID identifies an actor.
type PID struct{ _ int32 }

// ProcInfo describes an actor.
type ProcInfo struct{ Memory int }

// Spawn starts a new actor running f and returns its PID.
func Spawn(f func()) PID { panic("gogo only") }

// Self returns the calling actor's PID.
func Self() PID { panic("gogo only") }

// Send delivers msg to the actor to.
func Send(to PID, msg any) { panic("gogo only") }

// Receive blocks until a message arrives and returns it.
func Receive() any { panic("gogo only") }

// Info returns information about actor p.
func Info(p PID) ProcInfo { panic("gogo only") }
