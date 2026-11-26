// Package ir is the gogo infrared API: the "ir_tx" and "ir_rx" ports. It
// is a stub package like gogo/power: the compiler type-checks against it
// and the bodies (there are none) are never run. The C side is the board's
// IR driver (runtime/input/ir.c).
//
// Open a transmitter with port.Open("ir_tx", ir.TX{CarrierHz: 38000,
// DutyPct: 33}) and send port.Out; each is answered with port.Done. Open a
// receiver with port.Open("ir_rx", ir.RX{GapUs: 10000}); each frame arrives
// as one port.Chunk. The timings are little-endian uint16 microseconds,
// mark and space alternating, mark first, each at most 32767.
//
// The struct types TX and RX have fixed type indices 40 and 41 in every
// image (TYPE_IR_TX, TYPE_IR_RX in runtime/vm/opcodes.h); keep their field order in
// sync with compiler.reservedTypes.
package ir

// TX configures the transmitter: the carrier frequency in Hz (0 or less
// means 38000, else 10000..500000) and its duty cycle in percent (0 or less
// means 33, else at most 99); Open fails outside those ranges. An Out holds
// 1 to 256 timings (a received frame always fits). Each Out is answered
// with Done{Seq} when the frame has been sent, or Done{Seq: -1} when it was
// not: refused (bad timings, more than 256, or two already in flight) or
// the hardware could not send it. Closing the
// port forgets the queued Out and sends no Done; a frame already started is
// finished if it ends soon (on the ESP32-S3 boards: within 100 ms, which
// covers NEC), else cut short, since closing powers the emitter down.
// gogo/irproto builds the timings of known protocols.
type TX struct{ CarrierHz, DutyPct int }

// RX configures the receiver: a gap longer than GapUs microseconds ends a
// frame (0 or less means 10000, more than 32767 means 32767). Each frame is
// one port.Chunk whose Data holds its timings, ending with a mark, in the
// format TX takes, so it can be sent back as it is. A frame longer than
// 256 values is cut there and counts 1 in the Chunk's Dropped (on top of
// any Chunks lost before it, so a cut frame that follows a lost one shows
// 2). TimeMs is when the frame began if the board knows it, else when it was
// read, within 20 ms of its end.
type RX struct{ GapUs int }
