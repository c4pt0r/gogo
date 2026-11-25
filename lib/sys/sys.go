// Package sys is the gogo system API: the wall clock and randomness. It is a
// stub package like gogo/config: the compiler replaces every call with a VM
// builtin (B_UNIX_TIME, B_SET_UNIX_TIME, B_RANDOM), so the bodies are never
// executed. The C side is vm_unix_time_ms, vm_set_unix_time_ms and
// vm_random (runtime/vm/host.h), over the host's optional RTC and random hooks.
//
// The wall clock is separate from actor.Now (milliseconds since the VM
// started): it can be unknown. gogo/sntp sets it from the network; a TLS
// client needs it to check certificate validity.
//
// Trust. The wall clock is trusted image state: gogo has no privilege model
// (every image and module already has the file system and the network), so
// any code may call SetTime, and SNTP is unauthenticated. The only
// protection is a floor: the time the firmware or VM was built. SetTime
// refuses an earlier time, so no one can move the clock back before the
// build and revive a certificate that expired before it. Certificate
// validity is protected by that floor and nothing else.
package sys

// Time returns the Unix time in milliseconds (UTC), or 0 if it is not known:
// nothing set it yet and the platform has no clock of its own. Once known it
// keeps advancing by itself.
func Time() int64 { panic("gogo only") }

// SetTime makes ms (Unix time, milliseconds) the current time from now on
// and returns true. It returns false and changes nothing if ms is earlier
// than the build-time floor (see above). ms <= 0 forgets the time (Time
// returns 0 again) and returns true.
func SetTime(ms int64) bool { panic("gogo only") }

// Random returns 30 random bits (0 <= n < 1<<30) from the platform's entropy
// source (the hardware generator on the board, getentropy on unix). It is
// meant for protocol nonces, not as a key generator on its own.
func Random() int { panic("gogo only") }
