// Package kbd is the gogo keyboard API: the "kbd" port. It is a stub
// package like gogo/fs: the compiler type-checks against it and the bodies
// (there are none) are never run. The C side is the board's keyboard driver.
//
// Open it with port.Open("kbd", kbd.Config{RepeatDelayMs: 400, RepeatMs: 60}).
// One actor owns the port at a time. ok is false when the board has no
// keyboard. The owner receives a Key message for every key press, repeat
// and release. A Key that would take the owner past its memory limit is
// dropped (the owner is not killed).
//
// The struct types Config and Key have fixed type indices 29 and 30 in every
// image (TYPE_KBD_CONFIG, TYPE_KBD_KEY in runtime/vm/opcodes.h); keep their field
// order in sync with compiler.reservedTypes.
package kbd

// Config configures the port. A RepeatDelayMs of 0 turns auto-repeat off;
// RepeatMs is the interval between repeats (0 or less means 50).
type Config struct{ RepeatDelayMs, RepeatMs int }

// Key is one key event. Code is the USB HID usage of the key (the constants
// below for the non-letters), Char the character it types with the current
// modifiers (0 for none), Mods a bit set of the Mod constants, Down true
// for a press or a repeat and false for a release. A modifier key's own
// press carries its bit in Mods and its release does not.
type Key struct {
	Code, Char, Mods int
	Down             bool
}

// Modifier bits of Key.Mods.
const (
	ModCtrl  = 1
	ModShift = 2
	ModAlt   = 4
	ModOpt   = 8
	ModFn    = 16
)

// Key codes (USB HID usages; letters and digits use their HID usage too).
const (
	Enter     = 0x28
	Esc       = 0x29
	Backspace = 0x2A
	Tab       = 0x2B
	Space     = 0x2C
	F1        = 0x3A
	F2        = 0x3B
	F3        = 0x3C
	F4        = 0x3D
	F5        = 0x3E
	F6        = 0x3F
	F7        = 0x40
	F8        = 0x41
	F9        = 0x42
	F10       = 0x43
	F11       = 0x44
	F12       = 0x45
	Delete    = 0x4C
	Right     = 0x4F
	Left      = 0x50
	Down      = 0x51
	Up        = 0x52
	LeftCtrl  = 0xE0
	LeftShift = 0xE1
	LeftAlt   = 0xE2
	Opt       = 0xE3
	Fn        = 0xF0
)
