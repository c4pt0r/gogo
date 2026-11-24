package compiler

import (
	"encoding/binary"
	"fmt"
)

const (
	imageMagic   = 0x4F474F47
	imageVersion = 1
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

// ImageBuilder assembles an Image v1 byte slice.
type ImageBuilder struct {
	consts  []uint32
	strings map[string]uint32
	funcs   []imgFunc
	types   []imgType
	entry   uint32
}

// NewImageBuilder returns an empty builder.
func NewImageBuilder() *ImageBuilder {
	return &ImageBuilder{strings: map[string]uint32{}}
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

// SetEntry selects the entry function.
func (b *ImageBuilder) SetEntry(f uint32) { b.entry = f }

// Bytes serializes the image (little-endian words). Function code is laid out
// in index order and function-relative jump targets are made absolute.
func (b *ImageBuilder) Bytes() ([]byte, error) {
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
	out := make([]byte, 4*len(words))
	for i, w := range words {
		binary.LittleEndian.PutUint32(out[i*4:], w)
	}
	return out, nil
}
