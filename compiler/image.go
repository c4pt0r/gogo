package compiler

import (
	"encoding/binary"
	"fmt"
)

const (
	imageMagic = 0x4F474F47
	// imageVersion moves in lockstep with IMAGE_VERSION in runtime/vm/image.h: any
	// change to opcode numbers or semantics, builtin numbers, reserved
	// types or the term encoding bumps it (v2: M3; v3: M4; v4: M5 CALLV; v5: bitwise ops;
	// v6: B_FS and the gogo/fs reserved types; v7: the const map after the
	// code, see constMap; v8: reserved types kbd.*, power.*; v9: requires
	// bitmask and exports table after the const map, B_CODE and the
	// gogo/code reserved type Module; v10: the port stream types Chunk,
	// Out, Done and the gogo/imu and gogo/ir reserved types; v11: the
	// gogo/audio reserved types Mic and Speaker, requires bits mic and spk;
	// v12: the gogo/sys builtins B_UNIX_TIME, B_SET_UNIX_TIME and B_RANDOM,
	// 51-53).
	imageVersion = 12
)

// ImageVersion is the image format version this compiler emits.
const ImageVersion = imageVersion

// Index limits of an image (runtime/vm/term.h): at run time the top 11 bits of a
// const ref, a closure's function word and a module struct's type word are
// the module tag, so an image's const offsets have 17 bits (512 KB of
// constants), function indexes 21 and type indexes 16 (TYPEIS's basic
// types start at 0x10000).
const (
	MaxConstWords = 1 << 17
	MaxFuncs      = 1 << 21
	MaxTypes      = 1 << 16
)

type imgFunc struct {
	name           uint32 // const offset
	goName         string
	nParams, nRegs uint16
	code           []uint32
}

type imgType struct {
	name    uint32
	nFields uint32
}

// ImageBuilder assembles an image byte slice.
type ImageBuilder struct {
	consts  []uint32
	strings map[string]uint32
	int64s  map[int64]uint32
	funcs   []imgFunc
	types   []imgType
	entry   uint32
	// v9 trailer: capability bits, and the exported entries by name.
	requires uint32
	exports  []imgExport
}

type imgExport struct {
	name uint32 // const offset of the name string
	fn   uint32
}

// NewImageBuilder returns an empty builder.
func NewImageBuilder() *ImageBuilder {
	return &ImageBuilder{strings: map[string]uint32{}, int64s: map[int64]uint32{}}
}

// AddString adds a K_STRING constant (deduplicated) and returns its word offset.
func (b *ImageBuilder) AddString(s string) uint32 {
	if off, ok := b.strings[s]; ok {
		return off
	}
	off := uint32(len(b.consts))
	payload := (len(s) + 3) / 4
	b.consts = append(b.consts, HdrMake(uint32(2+payload), KString), uint32(len(s)))
	buf := make([]byte, payload*4)
	copy(buf, s)
	for i := 0; i < payload; i++ {
		b.consts = append(b.consts, binary.LittleEndian.Uint32(buf[i*4:]))
	}
	b.strings[s] = off
	return off
}

// AddInt64 adds a K_INT64 constant (deduplicated) and returns its word offset.
func (b *ImageBuilder) AddInt64(v int64) uint32 {
	if off, ok := b.int64s[v]; ok {
		return off
	}
	off := uint32(len(b.consts))
	b.consts = append(b.consts, HdrMake(3, KInt64), uint32(uint64(v)), uint32(uint64(v)>>32))
	b.int64s[v] = off
	return off
}

// AddFunc declares a function and returns its index.
func (b *ImageBuilder) AddFunc(name string, nParams, nRegs uint16) uint32 {
	n := b.AddString(name)
	b.funcs = append(b.funcs, imgFunc{name: n, goName: name, nParams: nParams, nRegs: nRegs})
	return uint32(len(b.funcs) - 1)
}

// SetFuncCode sets the code of function f. Functions are laid out in index
// order, so a function's code offset is the total length of earlier functions.
func (b *ImageBuilder) SetFuncCode(f uint32, code []uint32) { b.funcs[f].code = code }

// AddType declares a struct type and returns its index.
func (b *ImageBuilder) AddType(name string, nFields uint32) uint32 {
	n := b.AddString(name)
	b.types = append(b.types, imgType{name: n, nFields: nFields})
	return uint32(len(b.types) - 1)
}

// SetRequires sets the capability bits the image needs (Cap*).
func (b *ImageBuilder) SetRequires(bits uint32) { b.requires = bits }

// AddExport exports function f under name (the host spawns it by name).
func (b *ImageBuilder) AddExport(name string, f uint32) {
	b.exports = append(b.exports, imgExport{name: b.AddString(name), fn: f})
}

// SetEntry selects the entry function.
func (b *ImageBuilder) SetEntry(f uint32) { b.entry = f }

// Bytes serializes the image (little-endian words). Function code is laid out
// in index order and function-relative jump targets are made absolute.
func (b *ImageBuilder) Bytes() ([]byte, error) {
	if len(b.consts) > MaxConstWords {
		return nil, fmt.Errorf("image: const area is %d words, over the %d-word limit (strings and int64 constants)", len(b.consts), MaxConstWords)
	}
	if len(b.funcs) > MaxFuncs || len(b.types) > MaxTypes {
		return nil, fmt.Errorf("image: %d functions, %d types: over the limits (%d, %d)", len(b.funcs), len(b.types), MaxFuncs, MaxTypes)
	}
	var code []uint32
	var funcWords []uint32
	for fi, f := range b.funcs {
		if len(f.code) == 0 {
			return nil, fmt.Errorf("image: function %q (index %d) has no code", f.goName, fi)
		}
		base := uint32(len(code))
		reloc := append([]uint32(nil), f.code...)
		for pc := 0; pc < len(reloc); pc++ {
			op := uint8(reloc[pc])
			if !OpHasExt(op) {
				continue
			}
			if pc+1 >= len(reloc) {
				return nil, fmt.Errorf("image: function %q: missing ext word at %d", f.goName, pc)
			}
			if op == OpJmp || op == OpJmpf || op == OpJmpt {
				reloc[pc+1] += base
			}
			pc++
		}
		funcWords = append(funcWords, uint32(len(code)), uint32(f.nParams)|uint32(f.nRegs)<<16, f.name)
		code = append(code, reloc...)
	}
	words := []uint32{imageMagic, imageVersion, uint32(len(b.funcs)), uint32(len(b.types)),
		uint32(len(b.consts)), uint32(len(code)), b.entry, 0}
	words = append(words, funcWords...)
	for _, t := range b.types {
		words = append(words, t.nFields, t.name)
	}
	words = append(words, b.consts...)
	words = append(words, code...)
	words = append(words, constMap(b.consts)...)
	words = append(words, b.requires, uint32(len(b.exports)))
	for _, e := range b.exports {
		words = append(words, e.name, e.fn)
	}
	out := make([]byte, 4*len(words))
	for i, w := range words {
		binary.LittleEndian.PutUint32(out[i*4:], w)
	}
	return out, nil
}

// constMap is the v7 const map: one bit per const word (bit i%32 of word
// i/32), set where an object starts. The VM accepts a const ref only to a
// marked start, so a ref into an object's middle cannot forge another.
func constMap(consts []uint32) []uint32 {
	m := make([]uint32, (len(consts)+31)/32)
	for off := 0; off < len(consts); off += int(consts[off] >> 6) {
		m[off/32] |= 1 << (off % 32)
	}
	return m
}

// ImageRequires returns the capability word (Cap*) of a v9 image or module
// file: the word after the const map, see Bytes.
func ImageRequires(img []byte) (uint32, error) {
	if len(img) < 32 || len(img)%4 != 0 {
		return 0, fmt.Errorf("not a gogo image (size %d)", len(img))
	}
	w := func(i int) uint32 { return binary.LittleEndian.Uint32(img[i*4:]) }
	if w(0) != imageMagic {
		return 0, fmt.Errorf("not a gogo image (bad magic)")
	}
	if w(1) != imageVersion {
		return 0, fmt.Errorf("image version %d, this gogo writes version %d: rebuild it with `gogo build`", w(1), imageVersion)
	}
	// the requires word; uint64 so huge counts cannot wrap
	at := 8 + uint64(w(2))*3 + uint64(w(3))*2 + uint64(w(4)) + uint64(w(5)) + (uint64(w(4))+31)/32
	if at+2 > uint64(len(img)/4) {
		return 0, fmt.Errorf("truncated gogo image")
	}
	return w(int(at)), nil
}
