// Package actor is the gogo actor API. These stubs exist only so programs
// type-check with the standard Go toolchain; the gogo compiler replaces every
// call with VM instructions, so the bodies are never executed.
//
// The struct types ProcInfo, Reason, ExitMsg, DownMsg, Request and Options
// have fixed type indices 0-5 in every image (TYPE_* in vm/opcodes.h); keep
// their field order in sync with compiler.reservedTypes.
package actor

// PID identifies an actor.
type PID struct{ _ int32 }

// Ref is a VM-wide unique reference value; refs compare with == and !=.
type Ref struct{ _ [2]int32 }

// ProcInfo describes an actor.
type ProcInfo struct{ Memory int }

// Reason is why an actor exited.
type Reason struct {
	Code int    // one of the reason codes below, or a user code >= 100
	Func string // function a runtime error happened in, else ""
}

// ExitMsg is delivered to a linked actor that traps exits.
type ExitMsg struct {
	From   PID
	Reason Reason
}

// DownMsg is delivered to a monitoring actor when the monitored one exits.
type DownMsg struct {
	Ref    Ref
	From   PID
	Reason Reason
}

// Request is the message Call sends; answer it with Reply.
type Request struct {
	From PID
	Ref  Ref
	Body any
}

// Options configures SpawnOpt.
type Options struct {
	MaxMemory int // bytes; 0 means the platform default
}

// Reason codes (R_* in vm/opcodes.h). User codes start at 100.
const (
	Normal          = 0
	Killed          = 1
	OutOfMemory     = 2
	MailboxOverflow = 3
	NoProc          = 4
	DivZero         = 5
	BadCode         = 6
	Shutdown        = 7
	BadShift        = 8
)

// Spawn starts a new actor running f and returns its PID.
func Spawn(f func()) PID { panic("gogo only") }

// SpawnLink starts a new actor running f, linked to the caller.
func SpawnLink(f func()) PID { panic("gogo only") }

// SpawnOpt starts a new actor running f with the given options.
func SpawnOpt(f func(), opts Options) PID { panic("gogo only") }

// Self returns the calling actor's PID.
func Self() PID { panic("gogo only") }

// Send delivers msg to the actor to.
func Send(to PID, msg any) { panic("gogo only") }

// Receive blocks until a message arrives and returns it.
func Receive() any { panic("gogo only") }

// ReceiveTimeout waits at most ms milliseconds for a message; ok is false
// on timeout. ms == 0 only looks.
func ReceiveTimeout(ms int) (msg any, ok bool) { panic("gogo only") }

// Info returns information about actor p.
func Info(p PID) ProcInfo { panic("gogo only") }

// Link links the caller and p.
func Link(p PID) { panic("gogo only") }

// Unlink removes the link between the caller and p.
func Unlink(p PID) { panic("gogo only") }

// TrapExit turns exit signals into ExitMsg messages.
func TrapExit(on bool) { panic("gogo only") }

// Exit ends the calling actor with the given reason code.
func Exit(code int) { panic("gogo only") }

// Kill sends an exit signal with the given code to p.
func Kill(p PID, code int) { panic("gogo only") }

// Monitor watches p; a DownMsg carrying the returned Ref arrives when p exits.
func Monitor(p PID) Ref { panic("gogo only") }

// Demonitor cancels a monitor.
func Demonitor(r Ref) { panic("gogo only") }

// MakeRef returns a new unique Ref.
func MakeRef() Ref { panic("gogo only") }

// SendAfter sends msg to to after ms milliseconds.
func SendAfter(ms int, to PID, msg any) Ref { panic("gogo only") }

// CancelTimer cancels a SendAfter timer; false if it already fired.
func CancelTimer(r Ref) bool { panic("gogo only") }

// Sleep pauses the calling actor for ms milliseconds.
func Sleep(ms int) { panic("gogo only") }

// Now returns milliseconds since the VM started.
func Now() int64 { panic("gogo only") }

// Call sends a Request with body req to to and waits at most timeoutMs
// milliseconds for the reply; ok is false on timeout. If the server dies
// while the call is pending, Call does not return early: the caller waits
// until the timeout (monitor the server to notice its death sooner).
func Call(to PID, req any, timeoutMs int) (reply any, ok bool) { panic("gogo only") }

// Reply answers a Request.
func Reply(req Request, reply any) { panic("gogo only") }

// Register names p; false if the name is taken or p is not alive.
func Register(name string, p PID) bool { panic("gogo only") }

// Whereis looks a registered name up.
func Whereis(name string) (PID, bool) { panic("gogo only") }

// Unregister removes a name.
func Unregister(name string) { panic("gogo only") }
