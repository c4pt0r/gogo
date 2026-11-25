// Package irproto encodes and decodes infrared remote-control protocols on
// top of the raw timings of gogo/ir: little-endian uint16 microseconds, mark
// and space alternating, mark first. It is written in the gogo subset and
// compiled into every image that imports it; it does not touch a port, so
// EncodeNEC's result goes to an "ir_tx" port as port.Out{Data: ...} and a
// port.Chunk's Data from "ir_rx" goes to DecodeNEC.
//
// NEC: a 9000 µs leader mark and 4500 µs space, 32 bits LSB first (address,
// address inverted, command, command inverted), each a 560 µs mark and a
// 560 µs (0) or 1690 µs (1) space, and a trailing 560 µs mark: 67 values.
// Extended NEC sends a 16-bit address in place of the address and its
// inverse.
package irproto

import "gogo/bytes"

// NEC timings in microseconds, and the number of values in a frame.
const (
	NECLeaderMark  = 9000
	NECLeaderSpace = 4500
	NECMark        = 560
	NECZero        = 560
	NECOne         = 1690
	NECValues      = 67
)

// EncodeNEC returns the 67 timings of the NEC frame for addr and cmd. An
// addr of 0..255 is standard NEC (the address byte, then its inverse);
// 256..65535 is extended NEC (low byte, then high byte). Only the low 16
// bits of addr and the low 8 of cmd are used.
func EncodeNEC(addr, cmd int) bytes.Bytes {
	b := bytes.NewBuffer(2 * NECValues)
	for i := 0; i < NECValues; i++ {
		v := necValue(addr, cmd, i)
		b.WriteByte(byte(v & 255))
		b.WriteByte(byte(v >> 8))
	}
	return b.Freeze()
}

// DecodeNEC decodes an NEC frame: 67 timings (or 68, a trailing space
// ignored), each within ±25% of its nominal value, and the command byte
// followed by its inverse. addr is the address byte when the second byte is
// its inverse (standard NEC), else the 16-bit extended address; so an
// extended address whose high byte is the inverse of its low byte decodes
// as the standard address of its low byte. ok is false for anything else,
// such as a repeat code (9000, 2250, 560).
func DecodeNEC(b bytes.Bytes) (addr, cmd int, ok bool) {
	n := b.Len()
	if n != 2*NECValues && n != 2*NECValues+2 {
		return 0, 0, false
	}
	// A copy (134 bytes, dropped on return) so decodeNEC works on a []byte,
	// which plain `go test` can run; Bytes methods only run on the VM.
	p := make([]byte, n)
	b.CopyTo(p)
	return decodeNEC(p)
}

// necByte is payload byte k (0..3) of the NEC frame for addr and cmd.
func necByte(addr, cmd, k int) int {
	a := addr & 65535
	c := cmd & 255
	if k == 0 {
		return a & 255
	}
	if k == 1 {
		if a <= 255 {
			return 255 - a
		}
		return a >> 8
	}
	if k == 2 {
		return c
	}
	return 255 - c
}

// necValue is timing i (0..66) of the NEC frame for addr and cmd.
func necValue(addr, cmd, i int) int {
	if i == 0 {
		return NECLeaderMark
	}
	if i == 1 {
		return NECLeaderSpace
	}
	if i%2 == 0 {
		return NECMark // every bit's mark, and the trailing one
	}
	k := (i - 3) / 2 // bit 0..31
	if (necByte(addr, cmd, k/8)>>(k%8))&1 == 1 {
		return NECOne
	}
	return NECZero
}

// near reports whether v is within ±25% of nom.
func near(v, nom int) bool {
	return v*4 >= nom*3 && v*4 <= nom*5
}

// at is timing i of p (uint16 LE).
func at(p []byte, i int) int {
	return int(p[2*i]) | int(p[2*i+1])<<8
}

// decodeNEC is DecodeNEC on the timings p (67 or 68 values).
func decodeNEC(p []byte) (addr, cmd int, ok bool) {
	if len(p) != 2*NECValues && len(p) != 2*NECValues+2 {
		return 0, 0, false
	}
	if !near(at(p, 0), NECLeaderMark) || !near(at(p, 1), NECLeaderSpace) || !near(at(p, NECValues-1), NECMark) {
		return 0, 0, false
	}
	lo := 0 // bits 0..15: the address bytes
	hi := 0 // bits 16..31: the command bytes
	for k := 0; k < 32; k++ {
		if !near(at(p, 2+2*k), NECMark) {
			return 0, 0, false
		}
		s := at(p, 3+2*k)
		bit := 0
		if near(s, NECOne) {
			bit = 1
		} else if !near(s, NECZero) {
			return 0, 0, false
		}
		if k < 16 {
			lo = lo | bit<<k
		} else {
			hi = hi | bit<<(k-16)
		}
	}
	c := hi & 255
	if hi>>8 != 255-c {
		return 0, 0, false
	}
	a0 := lo & 255
	a1 := lo >> 8
	if a1 == 255-a0 {
		return a0, c, true
	}
	return lo, c, true
}
