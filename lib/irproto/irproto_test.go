//go:build gogohost

// Host tests of irproto.gg, run by tests/lib_test.go (TestLibHost) on a
// copy of lib/ as Go module gogo; the build tag keeps them out of the
// repository's module, where irproto.gg is not Go source.

package irproto

import (
	"math/rand"
	"testing"
)

// frame is the NEC frame for addr and cmd as uint16 LE bytes, each value
// scaled by scale(i) percent. (EncodeNEC itself returns a gogo bytes.Bytes,
// which only runs on the VM: the irdemo golden covers it.)
func frame(addr, cmd int, scale func(i int) int) []byte {
	p := make([]byte, 2*NECValues)
	for i := 0; i < NECValues; i++ {
		v := necValue(addr, cmd, i) * scale(i) / 100
		p[2*i] = byte(v)
		p[2*i+1] = byte(v >> 8)
	}
	return p
}

func exact(int) int { return 100 }

func TestNECTimings(t *testing.T) {
	p := frame(0x04, 0x08, exact)
	want := []int{9000, 4500, 560, 560, 560, 560, 560, 1690} // 0x04: bits 0 0 1 ...
	for i, w := range want {
		if at(p, i) != w {
			t.Fatalf("value %d = %d, want %d", i, at(p, i), w)
		}
	}
	if at(p, NECValues-1) != 560 {
		t.Fatalf("trailing mark %d", at(p, NECValues-1))
	}
	ones := 0
	for k := 0; k < 32; k++ {
		if at(p, 3+2*k) == NECOne {
			ones++
		}
	}
	if ones != 16 { // standard NEC: each byte and its inverse
		t.Fatalf("%d one bits, want 16", ones)
	}
}

func TestNECRoundTrip(t *testing.T) {
	for _, addr := range []int{0, 1, 0x04, 0x7F, 0xFF, 0x1234, 0xFFFF & 0xBF00, 0xABCD} {
		for cmd := 0; cmd < 256; cmd++ {
			a, c, ok := decodeNEC(frame(addr, cmd, exact))
			if !ok || a != addr || c != cmd {
				t.Fatalf("addr %#x cmd %#x: got %#x %#x %v", addr, cmd, a, c, ok)
			}
		}
	}
	// A trailing space (a frame replayed with one appended) is ignored.
	p := append(frame(7, 9, exact), 0x10, 0x27)
	if a, c, ok := decodeNEC(p); !ok || a != 7 || c != 9 {
		t.Fatalf("68 values: %d %d %v", a, c, ok)
	}
	// Bits above the protocol's are dropped.
	if a, c, ok := decodeNEC(frame(0x10004, 0x105, exact)); !ok || a != 4 || c != 5 {
		t.Fatalf("masked: %d %d %v", a, c, ok)
	}
}

func TestNECNoise(t *testing.T) {
	r := rand.New(rand.NewSource(1))
	for n := 0; n < 2000; n++ {
		addr, cmd := r.Intn(256), r.Intn(256)
		jitter := func(int) int { return 80 + r.Intn(41) } // ±20%: inside the tolerance
		if a, c, ok := decodeNEC(frame(addr, cmd, jitter)); !ok || a != addr || c != cmd {
			t.Fatalf("jitter: addr %d cmd %d: %d %d %v", addr, cmd, a, c, ok)
		}
	}
	for i := 0; i < NECValues; i++ { // one value 30% off fails the frame
		for _, pct := range []int{69, 131} {
			if _, _, ok := decodeNEC(frame(0x04, 0x08, func(j int) int {
				if j == i {
					return pct
				}
				return 100
			})); ok {
				t.Fatalf("value %d at %d%% accepted", i, pct)
			}
		}
	}
	for n := 0; n < 2000; n++ { // random timings
		p := make([]byte, 2*NECValues)
		for i := range p {
			p[i] = byte(r.Intn(256))
		}
		if _, _, ok := decodeNEC(p); ok {
			t.Fatalf("garbage accepted: %v", p)
		}
	}
	// A command byte not followed by its inverse.
	p := frame(0x04, 0x08, exact)
	s := 3 + 2*24 // bit 24: the inverted command's bit 0
	v := NECZero
	if at(p, s) == NECZero {
		v = NECOne
	}
	p[2*s], p[2*s+1] = byte(v), byte(v>>8)
	if _, _, ok := decodeNEC(p); ok {
		t.Fatal("bad command inverse accepted")
	}
	// The repeat code, and wrong lengths.
	rep := []byte{0x28, 0x23, 0xCA, 0x08, 0x30, 0x02}
	if _, _, ok := decodeNEC(rep); ok {
		t.Fatal("repeat code accepted")
	}
	// A full-length frame with the repeat code's 2250 µs leader space.
	p = frame(0x04, 0x08, exact)
	p[2], p[3] = byte(2250&255), byte(2250>>8)
	if _, _, ok := decodeNEC(p); ok {
		t.Fatal("2250 us leader space accepted")
	}
	// A broken address inverse reads as an extended address (documented).
	p = frame(0x04, 0x08, exact)
	s = 3 + 2*8 // bit 8: the inverted address's bit 0 (1 for 0x04)
	z := NECZero
	p[2*s], p[2*s+1] = byte(z), byte(z>>8)
	if a, c, ok := decodeNEC(p); !ok || a != 0x04|0xFA<<8 || c != 0x08 {
		t.Fatalf("broken address inverse: %#x %#x %v", a, c, ok)
	}
	if _, _, ok := decodeNEC(frame(1, 2, exact)[:2*NECValues-2]); ok {
		t.Fatal("short frame accepted")
	}
}
