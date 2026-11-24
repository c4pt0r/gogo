package compiler

import "fmt"

// Label is a jump target created by Asm.NewLabel.
type Label int

type fixup struct {
	at    int // index of the ext word to patch
	label Label
}

// Asm assembles one function's code. Jump targets are relative to the start
// of the function; ImageBuilder relocates them to absolute code-section
// offsets when it lays out the image.
type Asm struct {
	code   []uint32
	labels []int // word offset within code, or -1 while unbound
	fixups []fixup
}

// Len returns the number of words emitted so far.
func (a *Asm) Len() int { return len(a.code) }

func makeWord(op, ra, rb, rc uint8) uint32 {
	return uint32(op) | uint32(ra)<<8 | uint32(rb)<<16 | uint32(rc)<<24
}

// Emit appends an instruction without an ext word.
func (a *Asm) Emit(op, ra, rb, rc uint8) { a.code = append(a.code, makeWord(op, ra, rb, rc)) }

// EmitExt appends an instruction followed by its ext word.
func (a *Asm) EmitExt(op, ra, rb, rc uint8, ext uint32) {
	a.code = append(a.code, makeWord(op, ra, rb, rc), ext)
}

// Last returns the last word emitted (ok false if none).
func (a *Asm) Last() (w uint32, ok bool) {
	if len(a.code) == 0 {
		return 0, false
	}
	return a.code[len(a.code)-1], true
}

// SetLast replaces the last word emitted.
func (a *Asm) SetLast(w uint32) { a.code[len(a.code)-1] = w }

// DropLast removes the last word emitted, which must be a whole
// instruction without an ext word or fixup (the peepholes use it on CLEAR).
func (a *Asm) DropLast() { a.code = a.code[:len(a.code)-1] }

// NewLabel creates an unbound label.
func (a *Asm) NewLabel() Label {
	a.labels = append(a.labels, -1)
	return Label(len(a.labels) - 1)
}

// Bind binds l to the current position.
func (a *Asm) Bind(l Label) { a.labels[l] = len(a.code) }

// EmitJump emits JMP/JMPF/JMPT whose target is patched at Finish.
func (a *Asm) EmitJump(op uint8, ra uint8, l Label) {
	a.code = append(a.code, makeWord(op, ra, 0, 0), 0)
	a.fixups = append(a.fixups, fixup{len(a.code) - 1, l})
}

// Finish resolves labels and returns the code words.
func (a *Asm) Finish() ([]uint32, error) {
	for _, f := range a.fixups {
		pos := a.labels[f.label]
		if pos < 0 {
			return nil, fmt.Errorf("asm: label %d is never bound", f.label)
		}
		a.code[f.at] = uint32(pos)
	}
	return a.code, nil
}
