package compiler

import (
	"encoding/binary"
	"fmt"
	"math"
	"sort"
	"strings"
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
	// imageMinor is the newest feature minor. Renumbering a feature's
	// minor touches its constant here (floatMinor, sizedMinor, ifaceMinor,
	// refMinor),
	// IMAGE_MINOR in runtime/vm/image.h when it is the newest
	// (TestTermsMatchC checks both), and the "15.N" in docs and opcodes.h
	// comments.
	imageMinor = max(floatMinor, sizedMinor, ifaceMinor, refMinor)
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
// 15.0; 15.1 added screen.Config; 15.2 floating point (the float ops,
// B_FLOAT, and the K_FLOAT constants and TYPEIS ids, which scan counts
// apart: floatMinor); 15.3 sized integers (the unsigned ops, B_PRINTLNU
// and the new CONV modes: sizedMinor); 15.4 the builtins and reserved
// types of panic, interfaces and equality (ifaceMinor); 15.6 references
// (refMinor: SETFIELD and the type-row flags, which scan counts apart).
// A reserved type is keyed by its image name ("port.Chunk"); its
// entry is the minor of its current field layout, so an appended field
// raises it.
var (
	opMinor = map[uint8]uint16{
		OpAddf: floatMinor, OpSubf: floatMinor, OpMulf: floatMinor, OpDivf: floatMinor, OpNegf: floatMinor,
		OpEqf: floatMinor, OpNef: floatMinor, OpLtf: floatMinor, OpLef: floatMinor, OpConvf: floatMinor,
		OpDivu64: sizedMinor, OpRemu64: sizedMinor, OpLtu64: sizedMinor, OpLeu64: sizedMinor, OpShru64: sizedMinor,
		OpSetfield: refMinor,
	}
	builtinMinor = map[uint32]uint16{
		BFloat:    floatMinor,
		BPrintlnu: sizedMinor,
		BPanic:    ifaceMinor,
		BCmp:      ifaceMinor,
	}
	typeMinor = map[string]uint16{
		"screen.Config":      1, // 15.1: the "screen" port
		"errors.Code":        ifaceMinor,
		"errors.errorString": ifaceMinor,
	}
)

// floatMinor is the minor of floating point (15.2): an image with a float
// op, B_FLOAT, a K_FLOAT constant or a TYPEIS of float32 / float64 needs
// it, and requires FloatRequirement.
const floatMinor = 2

// sizedMinor is the minor of the sized integers (15.3): an image with an
// unsigned op, B_PRINTLNU, or a CONV mode they added (convMinor) needs
// it.
const sizedMinor = 3

// ifaceMinor is the minor of the language's abstraction phase (15.4:
// panic, interfaces, equality): every builtin and reserved type it added
// is stamped with it.
const ifaceMinor = 4

// refMinor is the minor of the language's reference phase (15.6:
// pointers, maps, read-only globals, fmt): SETFIELD, the per-type flags
// of a type row and a const struct (StructMut, StructMap, StructBox), the
// const slices and arrays, and B_SPRINT. 15.5 belongs to distributed
// actors.
const refMinor = 6

// convMinor is the minor of an OP_CONV with mode c (op): the modes 0-2
// are 15.0's.
func convMinor(op uint8, c uint8) uint16 {
	if op == OpConv && c > ConvByte {
		return sizedMinor
	}
	return 0
}

// FloatRequirement is the requirement name of an image that uses floating
// point: a VM built without GOGO_FLOAT lacks it and refuses the image.
const FloatRequirement = "float"

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
	consts   []uint32
	strings  map[string]uint32
	int64s   map[int64]uint32
	floats   map[uint64]uint32 // float64 constants by bits
	floats32 map[uint32]uint32 // float32 constants by bits
	structs  map[string]uint32 // const structs by type and fields
	// constSlices: the image has constant slices (15.6, refMinor)
	constSlices bool
	funcs       []imgFunc
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
	return &ImageBuilder{strings: map[string]uint32{}, int64s: map[int64]uint32{}, floats: map[uint64]uint32{}, floats32: map[uint32]uint32{}}
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

// AddFloat64 adds a float64 K_FLOAT constant (deduplicated by bits, so 0
// and -0 stay apart) and returns its word offset.
func (b *ImageBuilder) AddFloat64(v float64) uint32 {
	u := math.Float64bits(v)
	if off, ok := b.floats[u]; ok {
		return off
	}
	off := uint32(len(b.consts))
	b.consts = append(b.consts, HdrMake(3, KFloat), uint32(u), uint32(u>>32))
	b.floats[u] = off
	return off
}

// AddFloat32 adds a float32 K_FLOAT constant (deduplicated by bits) and
// returns its word offset.
func (b *ImageBuilder) AddFloat32(v float32) uint32 {
	u := math.Float32bits(v)
	if off, ok := b.floats32[u]; ok {
		return off
	}
	off := uint32(len(b.consts))
	b.consts = append(b.consts, HdrMake(2, KFloat), u)
	b.floats32[u] = off
	return off
}

// AddInt32 adds a K_INT constant: an int outside 31 bits in a constant
// object (an immediate does not need one).
func (b *ImageBuilder) AddInt32(v int32) uint32 {
	off := uint32(len(b.consts))
	b.consts = append(b.consts, HdrMake(2, KInt), uint32(v))
	return off
}

// AddTermSlice adds a constant slice (15.6: a package-level variable's)
// and returns its term: of the terms, or of the bytes if isBytes, in a
// K_ARRAY or K_BYTEARR of their length, behind a K_SLICE of that length
// and capacity (with no array when empty: an empty, non-nil slice).
func (b *ImageBuilder) AddTermSlice(terms []uint32, bytes []byte, isBytes bool) uint32 {
	n := len(terms)
	if isBytes {
		n = len(bytes)
	}
	arr := TermNil
	if n > 0 {
		off := uint32(len(b.consts))
		if isBytes {
			payload := (n + 3) / 4
			buf := make([]byte, payload*4)
			copy(buf, bytes)
			b.consts = append(b.consts, HdrMake(uint32(2+payload), KBytearr), uint32(n))
			for i := 0; i < payload; i++ {
				b.consts = append(b.consts, binary.LittleEndian.Uint32(buf[i*4:]))
			}
		} else {
			b.consts = append(b.consts, HdrMake(uint32(2+n), KArray), uint32(n))
			b.consts = append(b.consts, terms...)
		}
		arr = TermConst(off)
	}
	off := uint32(len(b.consts))
	b.consts = append(b.consts, HdrMake(5, KSlice), 0, uint32(n), uint32(n), arr)
	b.constSlices = true
	return TermConst(off)
}

// AddStruct adds a K_STRUCT constant of type typ (an image type index)
// with the given field terms (immediates or const refs), deduplicated,
// and returns its word offset.
func (b *ImageBuilder) AddStruct(typ uint32, fields []uint32) uint32 {
	key := fmt.Sprint(typ, fields)
	if off, ok := b.structs[key]; ok {
		return off
	}
	off := uint32(len(b.consts))
	b.consts = append(b.consts, HdrMake(uint32(2+len(fields)), KStruct), typ)
	b.consts = append(b.consts, fields...)
	if b.structs == nil {
		b.structs = map[string]uint32{}
	}
	b.structs[key] = off
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
	return b.AddTypeFlags(name, nFields, 0)
}

// AddTypeFlags is AddType for a type with flags (StructMut, StructMap,
// StructBox: 15.6), kept in its row's field-count word.
func (b *ImageBuilder) AddTypeFlags(name string, nFields, flags uint32) uint32 {
	n := b.AddString(name)
	b.types = append(b.types, imgType{name: n, nFields: nFields | flags, goName: name})
	return TypeOwnBase + uint32(len(b.types)-1)
}

// TypeFlags returns the flags of own type idx (0 for a reserved one).
func (b *ImageBuilder) TypeFlags(idx uint32) uint32 {
	if idx < TypeOwnBase {
		return 0
	}
	return b.types[idx-TypeOwnBase].nFields &^ 0xFFFF
}

// AddReservedType declares the next reserved type (index order) and
// returns its index. Its row is written only if the program uses it or a
// later reserved type. The names of the types up to screen.Config go into
// the const area here, whether written or not (images have always held
// them, first); those of later types only with their row (see Bytes), so
// that a new reserved type changes no image that does not carry it.
func (b *ImageBuilder) AddReservedType(name string, nFields uint32) uint32 {
	t := imgType{nFields: nFields, goName: name, name: noName}
	if len(b.reserved) <= int(TypeScreenConfig) {
		t.name = b.AddString(name)
	}
	b.reserved = append(b.reserved, t)
	return uint32(len(b.reserved) - 1)
}

// noName marks a reserved row whose name string is added only when the
// row is written (AddReservedType).
const noName = ^uint32(0)

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
	used, minor, usesFloat, err := b.scan(code)
	if err != nil {
		return nil, err
	}
	if usesFloat {
		b.requireFloat()
	}
	nRes := len(used)
	var rows []imgType
	for i, t := range b.reserved[:nRes] {
		if !used[i] {
			t.nFields = 0 // not used: any VM that knows the type accepts it
		}
		if t.name == noName {
			t.name = b.AddString(t.goName)
		}
		rows = append(rows, t)
	}
	if len(b.consts) > MaxConstWords { // the names just added
		return nil, fmt.Errorf("image: const area is %d words, over the %d-word limit (strings and int64 constants)", len(b.consts), MaxConstWords)
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
// builtins and used reserved types, and floatMinor if it uses floating
// point (usesFloat: a float op, B_FLOAT, a K_FLOAT constant or a TYPEIS of
// a float type). A row it carries but does not use is
// written with 0 fields and adds no minor: no VM reads it (NEWSTRUCT and
// const structs mark their type; TYPEIS and GETFIELD read no row), and
// the loader accepts any count up to its own.
func (b *ImageBuilder) scan(code []uint32) (used []bool, minor uint16, usesFloat bool, err error) {
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
		minor = max(minor, opMinor[op], convMinor(op, uint8(code[pc]>>24)))
		usesFloat = usesFloat || op >= OpAddf && op <= OpConvf
		if !OpHasExt(op) {
			continue
		}
		pc++
		switch op {
		case OpNewstruct, OpTypeis:
			mark(code[pc])
			if op == OpTypeis && (code[pc] == TyFloat32 || code[pc] == TyFloat64) {
				usesFloat = true
			}
		case OpBuiltin:
			minor = max(minor, builtinMinor[code[pc]])
			usesFloat = usesFloat || code[pc] == BFloat
		}
	}
	for _, t := range b.types {
		if t.nFields > 0xFFFF { // a row with flags
			minor = max(minor, refMinor)
		}
	}
	if b.constSlices {
		minor = max(minor, refMinor)
	}
	for off := 0; off < len(b.consts); off += int(b.consts[off] >> 6) {
		switch b.consts[off] & 63 {
		case KStruct:
			mark(b.consts[off+1] & 0xFFFF)
			if b.consts[off+1] > 0xFFFF { // a type word with flags
				minor = max(minor, refMinor)
			}
		case KFloat:
			usesFloat = true
		}
	}
	if usesFloat {
		minor = max(minor, floatMinor)
	}
	nRes := 0
	for t := range marked {
		nRes = max(nRes, int(t)+1)
	}
	if nRes > len(b.reserved) {
		return nil, 0, false, fmt.Errorf("image: reserved type %d used but not declared", nRes-1)
	}
	used = make([]bool, nRes)
	for t := range marked {
		used[t] = true
		minor = max(minor, typeMinor[b.reserved[t].goName])
	}
	return used, minor, usesFloat, nil
}

// requireFloat adds FloatRequirement to the image's requirements (once).
func (b *ImageBuilder) requireFloat() {
	off := b.AddString(FloatRequirement)
	for _, r := range b.requires {
		if r == off {
			return
		}
	}
	b.requires = append(b.requires, off)
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

// ImageInfo describes an image or module file, as read back from its
// bytes by ReadImageInfo (what `gogo info` prints).
type ImageInfo struct {
	Version    uint32 // the header's version word: major<<16 | minor (VersionString)
	Size       int    // bytes
	Funcs      int    // functions
	Types      int    // type rows: the reserved ones it carries, then its own
	ConstWords int    // words of constants
	CodeWords  int    // words of code
	Entry      string // the entry function's name
	// Requires are the drivers and mounts the image needs (see Requires;
	// soft //gogo:optional ones are not recorded), Exports the entries a
	// host or code.Spawn starts by name, both in image order.
	Requires []string
	Exports  []string
}

// ReadImageInfo reads the header and trailer of an image or module file.
// It checks the magic, the format version and the layout, not the code.
func ReadImageInfo(img []byte) (ImageInfo, error) {
	var info ImageInfo
	if len(img) < 32 || len(img)%4 != 0 {
		return info, fmt.Errorf("not a gogo image (size %d)", len(img))
	}
	w := func(i uint64) uint32 { return binary.LittleEndian.Uint32(img[i*4:]) }
	n := uint64(len(img) / 4)
	if w(0) != imageMagic {
		return info, fmt.Errorf("not a gogo image (bad magic)")
	}
	if err := CheckVersion(w(1)); err != nil {
		return info, err
	}
	// uint64 so huge counts cannot wrap
	nf, nt, nc, ncode := uint64(w(2)), uint64(w(3)), uint64(w(4)), uint64(w(5))
	cbase := 8 + nf*3 + nt*2
	at := cbase + nc + ncode + (nc+31)/32
	if at+1 > n || at+1+uint64(w(at)) > n || cbase+nc > n {
		return info, fmt.Errorf("truncated gogo image")
	}
	str := func(off uint64, what string, i uint64) (string, error) {
		if off+2 > nc {
			return "", fmt.Errorf("bad gogo image (%s %d)", what, i)
		}
		l := uint64(w(cbase + off + 1))
		if off+2+(l+3)/4 > nc {
			return "", fmt.Errorf("bad gogo image (%s %d)", what, i)
		}
		start := (cbase + off + 2) * 4
		return string(img[start : start+l]), nil
	}
	info = ImageInfo{Version: w(1), Size: len(img), Funcs: int(nf), Types: int(nt), ConstWords: int(nc), CodeWords: int(ncode)}
	for i := uint64(0); i < uint64(w(at)); i++ {
		s, err := str(uint64(w(at+1+i)), "requirement", i)
		if err != nil {
			return info, err
		}
		info.Requires = append(info.Requires, s)
	}
	at += 1 + uint64(w(at))
	if at+1 > n || at+1+2*uint64(w(at)) > n {
		return info, fmt.Errorf("truncated gogo image (exports)")
	}
	for i := uint64(0); i < uint64(w(at)); i++ {
		s, err := str(uint64(w(at+1+2*i)), "export", i)
		if err != nil {
			return info, err
		}
		info.Exports = append(info.Exports, s)
	}
	if e := uint64(w(6)); e < nf {
		info.Entry, _ = str(uint64(w(8+e*3+2)), "function", e)
	}
	return info, nil
}

// ImageRequires returns the requirement names of an image or module
// file: the trailer after the const map, see Bytes.
func ImageRequires(img []byte) ([]string, error) {
	info, err := ReadImageInfo(img)
	return info.Requires, err
}

// OptionalDrivers lists the drivers the program's packages name in their
// //gogo:optional directives, each as "name (package, ...)", sorted;
// those are soft, so the image does not record them.
func OptionalDrivers(p *Program) []string {
	by := map[string][]string{}
	for _, pkg := range p.packages() {
		for d := range pkg.Optional {
			by[d] = append(by[d], pkg.Pkg.Path())
		}
	}
	var out []string
	for d, pkgs := range by {
		sort.Strings(pkgs)
		out = append(out, d+" ("+strings.Join(pkgs, ", ")+")")
	}
	sort.Strings(out)
	return out
}
