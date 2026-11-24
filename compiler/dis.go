package compiler

import (
	"encoding/binary"
	"fmt"
	"strings"
)

// Disassemble renders an Image v1 as text.
func Disassemble(img []byte) (string, error) {
	if len(img)%4 != 0 || len(img) < 32 {
		return "", fmt.Errorf("dis: bad image size %d", len(img))
	}
	w := make([]uint32, len(img)/4)
	for i := range w {
		w[i] = binary.LittleEndian.Uint32(img[i*4:])
	}
	if w[0] != imageMagic || w[1] != imageVersion {
		return "", fmt.Errorf("dis: bad magic/version")
	}
	nf, nt, nc, ncode := int(w[2]), int(w[3]), int(w[4]), int(w[5])
	if 8+nf*3+nt*2+nc+ncode > len(w) {
		return "", fmt.Errorf("dis: truncated image")
	}
	funcs := w[8 : 8+nf*3]
	types := w[8+nf*3 : 8+nf*3+nt*2]
	consts := w[8+nf*3+nt*2 : 8+nf*3+nt*2+nc]
	code := w[8+nf*3+nt*2+nc : 8+nf*3+nt*2+nc+ncode]

	str := func(off uint32) (string, error) {
		if int(off)+2 > len(consts) || consts[off]&63 != KString {
			return "", fmt.Errorf("dis: bad string const %d", off)
		}
		n := int(consts[off+1])
		if int(off)+2+(n+3)/4 > len(consts) {
			return "", fmt.Errorf("dis: bad string const %d", off)
		}
		buf := make([]byte, (n+3)/4*4)
		for i := 0; i < (n+3)/4; i++ {
			binary.LittleEndian.PutUint32(buf[i*4:], consts[int(off)+2+i])
		}
		return string(buf[:n]), nil
	}
	funcName := func(i uint32) string {
		if int(i) < nf {
			if s, err := str(funcs[i*3+2]); err == nil {
				return s
			}
		}
		return fmt.Sprintf("func%d", i)
	}
	typeName := func(id uint32) string {
		switch id {
		case TyInt:
			return "TyInt"
		case TyBool:
			return "TyBool"
		case TyString:
			return "TyString"
		case TyPid:
			return "TyPid"
		case TyRef:
			return "TyRef"
		case TyInt64:
			return "TyInt64"
		}
		if int(id) < nt {
			if s, err := str(types[id*2+1]); err == nil {
				return s
			}
		}
		return fmt.Sprintf("type%d", id)
	}
	term := func(t uint32) string {
		switch {
		case t&1 == 1:
			return fmt.Sprint(int32(t) >> 1)
		case t == TermNil:
			return "nil"
		case t == TermTrue:
			return "true"
		case t == TermFalse:
			return "false"
		case t == TermEmptyStr:
			return "emptystr"
		case t&0xF == 0xA:
			if s, err := str(t >> 4); err == nil {
				return fmt.Sprintf("%q", s)
			}
			if off := int(t >> 4); off+3 <= len(consts) && consts[off] == HdrMake(3, KInt64) {
				return fmt.Sprintf("int64(%d)", int64(uint64(consts[off+1])|uint64(consts[off+2])<<32))
			}
		}
		return fmt.Sprintf("0x%08x", t)
	}

	var sb strings.Builder
	for fi := 0; fi < nf; fi++ {
		start := int(funcs[fi*3])
		end := ncode
		if fi+1 < nf {
			end = int(funcs[(fi+1)*3])
		}
		if start > end || end > ncode {
			return "", fmt.Errorf("dis: bad code range for func %d", fi)
		}
		fmt.Fprintf(&sb, "func %s (params=%d regs=%d):\n", funcName(uint32(fi)),
			funcs[fi*3+1]&0xFFFF, funcs[fi*3+1]>>16)
		for pc := start; pc < end; pc++ {
			ins := code[pc]
			op := uint8(ins)
			a, b, c := (ins>>8)&0xFF, (ins>>16)&0xFF, ins>>24
			if int(op) >= len(opNames) {
				return "", fmt.Errorf("dis: bad opcode %d at %d", op, pc)
			}
			var ext uint32
			if OpHasExt(op) {
				if pc+1 >= end {
					return "", fmt.Errorf("dis: missing ext word at %d", pc)
				}
				ext = code[pc+1]
			}
			var args string
			switch op {
			case OpNop:
			case OpMove, OpNeg, OpNot:
				args = fmt.Sprintf("r%d, r%d", a, b)
			case OpLoadk:
				args = fmt.Sprintf("r%d, %s", a, term(ext))
			case OpLoadi32:
				args = fmt.Sprintf("r%d, %d", a, int32(ext))
			case OpAdd, OpSub, OpMul, OpDiv, OpRem, OpEq, OpNe, OpLt, OpLe:
				args = fmt.Sprintf("r%d, r%d, r%d", a, b, c)
			case OpJmp:
				args = fmt.Sprintf("@%04d", ext)
			case OpJmpf, OpJmpt:
				args = fmt.Sprintf("r%d, @%04d", a, ext)
			case OpCall:
				args = fmt.Sprintf("r%d, r%d, %d, %s", a, b, c, funcName(ext))
			case OpRet:
				args = fmt.Sprintf("r%d", a)
			case OpNewstruct:
				args = fmt.Sprintf("r%d, r%d, %d, %s", a, b, c, typeName(ext))
			case OpGetfield, OpConv:
				args = fmt.Sprintf("r%d, r%d, %d", a, b, c)
			case OpMkclosure:
				args = fmt.Sprintf("r%d, r%d, %d, %s", a, b, c, funcName(ext))
			case OpTypeis:
				args = fmt.Sprintf("r%d, r%d, %s", a, b, typeName(ext))
			case OpBuiltin:
				name := fmt.Sprintf("builtin%d", ext)
				if int(ext) < len(builtinNames) {
					name = builtinNames[ext]
				}
				args = fmt.Sprintf("r%d, r%d, %d, %s", a, b, c, name)
			}
			line := fmt.Sprintf("  %04d  %s", pc, opNames[op])
			if args != "" {
				line += " " + args
			}
			sb.WriteString(line + "\n")
			if OpHasExt(op) {
				pc++
			}
		}
	}
	return sb.String(), nil
}
