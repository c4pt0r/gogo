// Package imu is the gogo motion-sensor API: the "imu" port. It is a stub
// package like gogo/power: the compiler type-checks against it and the
// bodies (there are none) are never run. The C side is the board's IMU
// driver (input/imu.c).
//
// Open it with port.Open("imu", imu.Config{RateHz: 100, Gestures: imu.Tap |
// imu.DoubleTap, BatchMs: 200}). The owner receives port.Chunk messages
// (batches of samples, none when RateHz is 0) and Gesture messages.
//
// A sample is 12 bytes, little-endian int16: gx gy gz ax ay az. The ranges
// are fixed: acceleration +-8 g, rotation +-2000 dps (both over +-32768).
// The axes are the sensor chip's own, which differ between boards.
//
// The struct types Config and Gesture have fixed type indices 38 and 39 in
// every image (TYPE_IMU_CONFIG, TYPE_IMU_GESTURE in vm/opcodes.h); keep
// their field order in sync with compiler.reservedTypes.
package imu

// Config configures the port. RateHz is 0 (no samples, gestures only), 25,
// 50, 100 or 200; Gestures is a mask of the gesture constants below; BatchMs
// (default 200) is how long samples are gathered into one Chunk: 0 or less
// means 200, any other value is clamped to 50..1000.
//
// A Chunk's TimeMs is the start of its batch window (the open, or the
// previous Chunk), within one poll period (50 ms) of its first sample. A
// partial last batch is discarded when the port is closed or its owner
// exits, so close right after a Chunk to lose nothing.
type Config struct{ RateHz, Gestures, BatchMs int }

// Gesture is an event: Kind is one of the gesture constants below; Value is
// the orientation number for Orientation, the step count for Step, else 0.
// The orientation number's low two bits are 0 portrait upright, 1
// landscape left, 2 portrait upside down, 3 landscape right; 4 is added
// when the board is face down. The step count is cumulative since the
// sensor was set up (closing the port does not reset it).
type Gesture struct{ Kind, Value int }

// Gestures: the bits of Config.Gestures, and the Kind of a Gesture.
const (
	Tap         = 1
	DoubleTap   = 2
	TripleTap   = 4
	Orientation = 8
	Flat        = 16
	AnyMotion   = 32
	NoMotion    = 64
	Step        = 128
)
