package compiler

import (
	"encoding/binary"
	"fmt"
)

const (
	imageMagic = 0x4F474F47
	// imageMajor and imageMinor move in lockstep with IMAGE_MAJOR and
	// IMAGE_MINOR in runtime/vm/image.h (TestImageVersionMatchesC). The
	// header's version word is major<<16 | minor; a VM runs the images of
	// its major whose minor is at most its own. imageMinor is the newest
	// minor this compiler knows; each image is stamped with the lowest
	// minor its program needs (see the minor tables below). A new builtin,
	// opcode, reserved type or appended int field raises imageMinor and
	// gets an entry in its table; anything incompatible bumps imageMajor
	// and resets imageMinor and the tables (single-number versions 1 to
	// 14 came before: see runtime/vm/image.h).
	imageMajor = 15
	imageMinor = 0
)

// ImageMajor and ImageMinor are the image format this compiler emits
// (the newest minor it can stamp).
const (
	ImageMajor = imageMajor
	ImageMinor = imageMinor
)

// The image format minor in which each addition after 15.0 appeared: an
// image's minor is the highest one among the opcodes, builtins and
// reserved types it uses (and every reserved row it carries). Empty at
// 15.0. A reserved type is keyed by its image name ("port.Chunk"); its
// entry is the minor of its current field layout, so an appended field
// raises it.
var (
	opMinor      = map[uint8]uint16{}
	builtinMinor = map[uint32]uint16{}
	typeMinor    = map[string]uint16{}
)

// VersionString renders an image version word as "major.minor".
func VersionString(w uint32) string { return fmt.Sprintf("%d.%d", w>>16, w&0xFFFF) }

// CheckVersion reports whether a VM of this compiler's format runs an image
// with version word w: the same major and a minor up to imageMinor.
func CheckVersion(w uint32) error {
	if w>>16 == imageMajor && w&0xFFFF <= imageMinor {
		return nil
	}
	return fmt.Errorf("image format %s, this gogo runs %d.0 to %d.%d: rebuild it with `gogo build`",
		VersionString(w), imageMajor, imageMajor, imageMinor)
}

// Index limits of an image (runtime/vm/term.h): at run time the top 11 bits of a
// const ref, a closure's function word and a module struct's type word are
// the module tag, so an image's const offsets have 17 bits (512 KB of
// constants), function indexes 21 and type indexes 16 (TYPEIS's basic
// types start at 0x10000; own types start at TypeOwnBase).
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
	goName  string
}

// ImageBuilder assembles an image byte slice.
type ImageBuilder struct {
	consts  []uint32
	strings map[string]uint32
	int64s  map[int64]uint32
	funcs   []imgFunc
	// reserved holds a row for every reserved type (AddReservedType, in
	// index order); only the first nReserved are written, where
	// nReserved is one more than the highest reserved type the program
	// uses (UseType, or named by a NEWSTRUCT, TYPEIS or const struct).
	// A row the program does not use is written with 0 fields, so it
	// asks nothing of the VM (see scan).
	reserved []imgType
	used     map[uint32]bool // reserved indexes UseType marked
	types    []imgType       // own types: index TypeOwnBase + position
	entry    uint32
	// The trailer: requirement names (const offsets), and the exported
	// entries by name.
	requires []uint32
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

// AddType declares an own struct type and returns its index (TypeOwnBase
// and up, in declaration order).
func (b *ImageBuilder) AddType(name string, nFields uint32) uint32 {
	n := b.AddString(name)
	b.types = append(b.types, imgType{name: n, nFields: nFields, goName: name})
	return TypeOwnBase + uint32(len(b.types)-1)
}

// AddReservedType declares the next reserved type (index order) and
// returns its index. Its row is written only if the program uses it or a
// later reserved type.
func (b *ImageBuilder) AddReservedType(name string, nFields uint32) uint32 {
	n := b.AddString(name)
	b.reserved = append(b.reserved, imgType{name: n, nFields: nFields, goName: name})
	return uint32(len(b.reserved) - 1)
}

// UseType marks type idx as used by the program: a reserved type then
// gets its row with its fields (and every lower one, with none) in the
// image, and its minor counts.
func (b *ImageBuilder) UseType(idx uint32) {
	if idx < TypeOwnBase {
		if b.used == nil {
			b.used = map[uint32]bool{}
		}
		b.used[idx] = true
	}
}

// SetRequires sets the names the image requires from the board: port
// drivers and mount prefixes (see Requires).
func (b *ImageBuilder) SetRequires(names []string) {
	b.requires = nil
	for _, n := range names {
		b.requires = append(b.requires, b.AddString(n))
	}
}

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
	if len(b.funcs) > MaxFuncs || len(b.types) > int(MaxTypes-TypeOwnBase) {
		return nil, fmt.Errorf("image: %d functions, %d types: over the limits (%d, %d)", len(b.funcs), len(b.types), MaxFuncs, MaxTypes-TypeOwnBase)
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
	used, minor, err := b.scan(code)
	if err != nil {
		return nil, err
	}
	nRes := len(used)
	var rows []imgType
	for i, t := range b.reserved[:nRes] {
		if !used[i] {
			t.nFields = 0 // not used: any VM that knows the type accepts it
		}
		rows = append(rows, t)
	}
	rows = append(rows, b.types...)
	words := []uint32{imageMagic, imageMajor<<16 | uint32(minor), uint32(len(b.funcs)), uint32(len(rows)),
		uint32(len(b.consts)), uint32(len(code)), b.entry, uint32(nRes)}
	words = append(words, funcWords...)
	for _, t := range rows {
		words = append(words, t.nFields, t.name)
	}
	words = append(words, b.consts...)
	words = append(words, code...)
	words = append(words, constMap(b.consts)...)
	words = append(words, uint32(len(b.requires)))
	words = append(words, b.requires...)
	words = append(words, uint32(len(b.exports)))
	for _, e := range b.exports {
		words = append(words, e.name, e.fn)
	}
	out := make([]byte, 4*len(words))
	for i, w := range words {
		binary.LittleEndian.PutUint32(out[i*4:], w)
	}
	return out, nil
}

// scan finds the reserved types the program uses (those UseType marked,
// and those a NEWSTRUCT, TYPEIS or const struct names): used[i] for every
// reserved row the image carries (len(used) is n_reserved). And the lowest
// format minor that runs the image: the highest among its opcodes,
// builtins and used reserved types. A row it carries but does not use is
// written with 0 fields and adds no minor: no VM reads it (NEWSTRUCT and
// const structs mark their type; TYPEIS and GETFIELD read no row), and
// the loader accepts any count up to its own.
func (b *ImageBuilder) scan(code []uint32) (used []bool, minor uint16, err error) {
	marked := map[uint32]bool{}
	for t := range b.used {
		marked[t] = true
	}
	mark := func(t uint32) {
		if t < TypeOwnBase {
			marked[t] = true
		}
	}
	for pc := 0; pc < len(code); pc++ {
		op := uint8(code[pc])
		minor = max(minor, opMinor[op])
		if !OpHasExt(op) {
			continue
		}
		pc++
		switch op {
		case OpNewstruct, OpTypeis:
			mark(code[pc])
		case OpBuiltin:
			minor = max(minor, builtinMinor[code[pc]])
		}
	}
	for off := 0; off < len(b.consts); off += int(b.consts[off] >> 6) {
		if b.consts[off]&63 == KStruct {
			mark(b.consts[off+1])
		}
	}
	nRes := 0
	for t := range marked {
		nRes = max(nRes, int(t)+1)
	}
	if nRes > len(b.reserved) {
		return nil, 0, fmt.Errorf("image: reserved type %d used but not declared", nRes-1)
	}
	used = make([]bool, nRes)
	for t := range marked {
		used[t] = true
		minor = max(minor, typeMinor[b.reserved[t].goName])
	}
	return used, minor, nil
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

// ImageRequires returns the requirement names of an image or module
// file: the trailer after the const map, see Bytes.
func ImageRequires(img []byte) ([]string, error) {
	if len(img) < 32 || len(img)%4 != 0 {
		return nil, fmt.Errorf("not a gogo image (size %d)", len(img))
	}
	w := func(i uint64) uint32 { return binary.LittleEndian.Uint32(img[i*4:]) }
	n := uint64(len(img) / 4)
	if w(0) != imageMagic {
		return nil, fmt.Errorf("not a gogo image (bad magic)")
	}
	if err := CheckVersion(w(1)); err != nil {
		return nil, err
	}
	// uint64 so huge counts cannot wrap
	nf, nt, nc, ncode := uint64(w(2)), uint64(w(3)), uint64(w(4)), uint64(w(5))
	cbase := 8 + nf*3 + nt*2
	at := cbase + nc + ncode + (nc+31)/32
	if at+1 > n || at+1+uint64(w(at)) > n || cbase+nc > n {
		return nil, fmt.Errorf("truncated gogo image")
	}
	var names []string
	for i := uint64(0); i < uint64(w(at)); i++ {
		off := uint64(w(at + 1 + i))
		if off+2 > nc {
			return nil, fmt.Errorf("bad gogo image (requirement %d)", i)
		}
		l := uint64(w(cbase + off + 1))
		if off+2+(l+3)/4 > nc {
			return nil, fmt.Errorf("bad gogo image (requirement %d)", i)
		}
		start := (cbase + off + 2) * 4
		names = append(names, string(img[start:start+l]))
	}
	return names, nil
}
