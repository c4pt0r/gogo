package compiler

import (
	"encoding/binary"
	"os"
	"regexp"
	"strings"
	"testing"
)

func goName(prefix, suffix string) string {
	return prefix + strings.ToUpper(suffix[:1]) + strings.ToLower(strings.ReplaceAll(suffix, "_", ""))[1:]
}

func TestOpcodesMatchC(t *testing.T) {
	src, err := os.ReadFile("../vm/opcodes.h")
	if err != nil {
		t.Fatal(err)
	}
	text := string(src)
	cConsts := map[string]uint32{}
	add := func(re, prefix string, base int) {
		for _, m := range regexp.MustCompile(re).FindAllStringSubmatch(text, -1) {
			var v uint64
			digits := m[2]
			for _, ch := range digits {
				d := strings.IndexRune("0123456789abcdef", ch|0x20)
				v = v*uint64(base) + uint64(d)
			}
			cConsts[goName(prefix, m[1])] = uint32(v)
		}
	}
	add(`\bOP_(\w+)\s*=\s*(\d+)`, "Op", 10)
	add(`\bB_(\w+)\s*=\s*(\d+)`, "B", 10)
	add(`#define\s+TY_(\w+)\s+0x([0-9a-fA-F]+)`, "Ty", 16)
	if len(cConsts) < 30 {
		t.Fatalf("parsed only %d C constants", len(cConsts))
	}
	for n, v := range cConsts {
		if g, ok := goConstants[n]; !ok || g != v {
			t.Errorf("%s: C=%d Go=%d (present=%v)", n, v, g, ok)
		}
	}
	for n := range goConstants {
		if _, ok := cConsts[n]; !ok {
			t.Errorf("%s has no C twin", n)
		}
	}
	if len(opNames) != int(OpBuiltin)+1 || len(builtinNames) != int(BMemory)+1 {
		t.Error("name tables out of sync")
	}
	// ext table vs C op_has_ext.
	body := regexp.MustCompile(`(?s)op_has_ext.*?return true;`).FindString(text)
	cExt := map[string]bool{}
	for _, m := range regexp.MustCompile(`case OP_(\w+):`).FindAllStringSubmatch(body, -1) {
		cExt[goName("Op", m[1])] = true
	}
	for n, v := range goConstants {
		if strings.HasPrefix(n, "Op") && OpHasExt(uint8(v)) != cExt[n] {
			t.Errorf("OpHasExt(%s)=%v, C says %v", n, OpHasExt(uint8(v)), cExt[n])
		}
	}
}

func TestAsmLabels(t *testing.T) {
	a := &Asm{}
	top := a.NewLabel()
	end := a.NewLabel()
	a.Bind(top)
	a.EmitJump(OpJmpf, 1, end) // words 0,1
	a.Emit(OpNop, 0, 0, 0)     // word 2
	a.EmitJump(OpJmp, 0, top)  // words 3,4
	a.Bind(end)
	a.Emit(OpRet, 0, 0, 0)
	code, err := a.Finish()
	if err != nil {
		t.Fatal(err)
	}
	if code[1] != 5 || code[4] != 0 || len(code) != 6 {
		t.Fatalf("bad fixups: %v", code)
	}
	b := &Asm{}
	b.EmitJump(OpJmp, 0, b.NewLabel())
	if _, err := b.Finish(); err == nil {
		t.Fatal("expected unbound label error")
	}
}

func words(b []byte) []uint32 {
	w := make([]uint32, len(b)/4)
	for i := range w {
		w[i] = binary.LittleEndian.Uint32(b[i*4:])
	}
	return w
}

func TestImageLayout(t *testing.T) {
	b := NewImageBuilder()
	f := b.AddFunc("main", 0, 1)
	b.SetFuncCode(f, []uint32{uint32(OpNop)})
	b.AddType("P", 2)
	b.SetEntry(f)
	if b.AddString("main") != b.AddString("main") {
		t.Fatal("strings not deduplicated")
	}
	raw, err := b.Bytes()
	if err != nil {
		t.Fatal(err)
	}
	w := words(raw)
	// "main": 2 hdr words + 1 payload = 3; "P": 3.
	want := []uint32{0x4F474F47, 1, 1, 1, 6, 1, 0, 0}
	for i, v := range want {
		if w[i] != v {
			t.Fatalf("header word %d = %#x, want %#x", i, w[i], v)
		}
	}
	nameOff := w[8+2]
	consts := w[8+3+2:]
	if consts[nameOff] != HdrMake(3, KString) || consts[nameOff+1] != 4 ||
		string(binary.LittleEndian.AppendUint32(nil, consts[nameOff+2])) != "main" {
		t.Fatalf("bad name string at %d", nameOff)
	}
	if w[8+3] != 2 { // type n_fields
		t.Fatal("type fields")
	}
}

func TestDisassemble(t *testing.T) {
	b := NewImageBuilder()
	f := b.AddFunc("main", 0, 2)
	var a Asm
	a.EmitExt(OpLoadk, 0, 0, 0, TermInt(5))
	a.Emit(OpAdd, 1, 0, 0)
	a.Emit(OpRet, 1, 0, 0)
	code, _ := a.Finish()
	b.SetFuncCode(f, code)
	b.SetEntry(f)
	raw, err := b.Bytes()
	if err != nil {
		t.Fatal(err)
	}
	got, err := Disassemble(raw)
	if err != nil {
		t.Fatal(err)
	}
	want := "func main (params=0 regs=2):\n" +
		"  0000  LOADK r0, 5\n" +
		"  0002  ADD r1, r0, r0\n" +
		"  0003  RET r1\n"
	if got != want {
		t.Fatalf("got:\n%s\nwant:\n%s", got, want)
	}
}

func TestDisassembleOperands(t *testing.T) {
	b := NewImageBuilder()
	f := b.AddFunc("fib", 1, 4)
	ty := b.AddType("P", 2)
	s := b.AddString("hello")
	var a Asm
	l := a.NewLabel()
	a.EmitExt(OpLoadk, 0, 0, 0, TermNil)
	a.EmitExt(OpLoadk, 0, 0, 0, TermConst(s))
	a.EmitExt(OpCall, 0, 1, 1, f)
	a.EmitJump(OpJmpf, 3, l)
	a.EmitExt(OpNewstruct, 2, 0, 2, ty)
	a.EmitExt(OpTypeis, 2, 0, 0, TyInt)
	a.Bind(l)
	a.EmitExt(OpBuiltin, 0, 1, 2, BPrintln)
	code, _ := a.Finish()
	b.SetFuncCode(f, code)
	raw, err := b.Bytes()
	if err != nil {
		t.Fatal(err)
	}
	got, err := Disassemble(raw)
	if err != nil {
		t.Fatal(err)
	}
	for _, w := range []string{"LOADK r0, nil", `LOADK r0, "hello"`, "CALL r0, r1, 1, fib",
		"JMPF r3, @0012", "NEWSTRUCT r2, r0, 2, P", "TYPEIS r2, r0, TyInt", "BUILTIN r0, r1, 2, PRINTLN"} {
		if !strings.Contains(got, w) {
			t.Errorf("missing %q in:\n%s", w, got)
		}
	}
}

func jumpyCode(t *testing.T) []uint32 {
	var a Asm
	top, end := a.NewLabel(), a.NewLabel()
	a.Bind(top)
	a.EmitJump(OpJmpf, 1, end)
	a.Emit(OpNop, 0, 0, 0)
	a.EmitJump(OpJmp, 0, top)
	a.Bind(end)
	a.Emit(OpRet, 0, 0, 0)
	code, err := a.Finish()
	if err != nil {
		t.Fatal(err)
	}
	return code
}

func TestImageRelocatesJumps(t *testing.T) {
	// f0 has 3 words, f1 (jumpy) starts at 3: targets 3 and 8.
	// Code is set out of index order.
	b := NewImageBuilder()
	f0 := b.AddFunc("a", 0, 1)
	f1 := b.AddFunc("b", 0, 2)
	b.SetFuncCode(f1, jumpyCode(t))
	b.SetFuncCode(f0, []uint32{uint32(OpNop), uint32(OpNop), uint32(OpRet)})
	raw, err := b.Bytes()
	if err != nil {
		t.Fatal(err)
	}
	got, err := Disassemble(raw)
	if err != nil {
		t.Fatal(err)
	}
	for _, w := range []string{"0003  JMPF r1, @0008", "0006  JMP @0003", "0008  RET r0"} {
		if !strings.Contains(got, w) {
			t.Errorf("missing %q in:\n%s", w, got)
		}
	}
}

func TestImageEmptyFuncRejected(t *testing.T) {
	b := NewImageBuilder()
	b.AddFunc("x", 0, 1)
	if _, err := b.Bytes(); err == nil {
		t.Fatal("expected error for func without code")
	}
}
