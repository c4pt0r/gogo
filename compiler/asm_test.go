package compiler

import (
	"encoding/binary"
	"go/ast"
	"go/constant"
	"go/token"
	"go/types"
	"os"
	"regexp"
	"strconv"
	"strings"
	"testing"
	"unicode"

	"gogotools/lib"
)

// goName maps a C constant to its Go twin: OP_LOADK -> OpLoadk,
// B_MAKE_REF -> BMakeRef.
func goName(prefix, suffix string) string {
	name := prefix
	for _, part := range strings.Split(suffix, "_") {
		if part != "" {
			name += part[:1] + strings.ToLower(part[1:])
		}
	}
	return name
}

func TestOpcodesMatchC(t *testing.T) {
	src, err := os.ReadFile("../runtime/vm/opcodes.h")
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
	add(`#define\s+TYPE_(\w+)\s+(\d+)`, "Type", 10)
	if len(cConsts) < 30 {
		t.Fatalf("parsed only %d C constants", len(cConsts))
	}
	for _, n := range []string{"BPortOpen", "BPortClose", "TypeGpioConfig", "TypeGpioWrite", "TypeGpioEdge", "TypeClosed", "OpClear", "OpRetn", "OpCallv", "BBytesCopyto", "BPortHandoff", "BNetSubscribe", "BConfigGet", "TypeNetDatagram", "TyBytes"} {
		if _, ok := cConsts[n]; !ok {
			t.Errorf("%s not parsed from runtime/vm/opcodes.h", n)
		}
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
	if len(opNames) != int(OpSetfield)+1 || len(builtinNames) != int(BCmp)+1 {
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

// TestReasonCodesMatchC cross-checks the gogo/actor reason constants with
// the C R_* enum: actor.OutOfMemory <-> R_OUT_OF_MEMORY.
func TestReasonCodesMatchC(t *testing.T) {
	src, err := os.ReadFile("../runtime/vm/opcodes.h")
	if err != nil {
		t.Fatal(err)
	}
	cCodes := map[string]int64{}
	for _, m := range regexp.MustCompile(`\bR_(\w+)\s*=\s*(\d+)`).FindAllStringSubmatch(string(src), -1) {
		v, _ := strconv.ParseInt(m[2], 10, 64)
		cCodes[m[1]] = v
	}
	pkg, err := testLoader().lib(ActorPath)
	if err != nil {
		t.Fatal(err)
	}
	goCodes := map[string]int64{}
	for _, name := range pkg.Scope().Names() {
		if c, ok := pkg.Scope().Lookup(name).(*types.Const); ok {
			v, _ := constant.Int64Val(c.Val())
			goCodes[upperSnake(name)] = v
		}
	}
	if len(goCodes) != 13 {
		t.Errorf("gogo/actor has %d constants, want the 12 reason codes and Failed", len(goCodes))
	}
	for n, v := range goCodes {
		if cv, ok := cCodes[n]; !ok || cv != v {
			t.Errorf("R_%s: Go=%d C=%d (present=%v)", n, v, cv, ok)
		}
	}
	for n := range cCodes {
		if _, ok := goCodes[n]; !ok {
			t.Errorf("R_%s has no Go twin", n)
		}
	}
}

// upperSnake turns OutOfMemory into OUT_OF_MEMORY.
func upperSnake(s string) string {
	var b strings.Builder
	for i, r := range s {
		if i > 0 && unicode.IsUpper(r) {
			b.WriteByte('_')
		}
		b.WriteRune(unicode.ToUpper(r))
	}
	return b.String()
}

// TestReservedTypesMatchStubs checks the reserved type table against the
// gogo/actor and gogo/port stubs (names, field names in order) and the
// TYPE_* indices.
func TestReservedTypesMatchStubs(t *testing.T) {
	l := testLoader()
	pkgs := map[string]*types.Package{}
	for _, path := range []string{ActorPath, PortPath, GpioPath, NetPath, FsPath, KbdPath, PowerPath, CodePath, ImuPath, IrPath, AudioPath, ScreenPath, ErrorsPath} {
		pkg, err := l.lib(path)
		if err != nil {
			t.Fatal(err)
		}
		pkgs[path] = pkg
	}
	// Types are only ever appended (later milestones add port.Chunk after
	// code.Module), so packages are not grouped; every package must be known.
	for i, rt := range reservedTypes {
		if pkgs[rt.pkg] == nil {
			t.Errorf("reserved type %s: unknown package %s", rt.name, rt.pkg)
			continue
		}
		prefix := "Type"
		switch rt.pkg {
		case GpioPath:
			prefix = "TypeGpio"
		case NetPath:
			prefix = "TypeNet"
		case FsPath:
			prefix = "TypeFs"
		case KbdPath:
			prefix = "TypeKbd"
		case PowerPath:
			prefix = "TypePower"
		case CodePath:
			prefix = "TypeCode"
		case ImuPath:
			prefix = "TypeImu"
		case IrPath:
			prefix = "TypeIr"
		case AudioPath:
			prefix = "TypeAudio"
		case ScreenPath:
			prefix = "TypeScreen"
		case ErrorsPath:
			prefix = "TypeErrors"
		}
		if g, ok := goConstants[prefix+strings.ToUpper(rt.name[:1])+strings.ToLower(rt.name[1:])]; !ok || g != uint32(i) {
			t.Errorf("%s.%s is reserved index %d, TYPE_ constant says %d (present=%v)", rt.pkg, rt.name, i, g, ok)
		}
		obj, ok := pkgs[rt.pkg].Scope().Lookup(rt.name).(*types.TypeName)
		if !ok {
			t.Errorf("%s.%s missing from the stubs", rt.pkg, rt.name)
			continue
		}
		var names []string
		if st, ok := obj.Type().Underlying().(*types.Struct); ok {
			for j := 0; j < st.NumFields(); j++ {
				names = append(names, st.Field(j).Name())
			}
		} else {
			names = []string{"value"} // the box of a named basic type
		}
		if strings.Join(names, " ") != strings.Join(rt.fields, " ") {
			t.Errorf("%s.%s fields %v, want %v", rt.pkg, rt.name, names, rt.fields)
		}
	}
	if len(reservedTypes) != 47 {
		t.Errorf("%d reserved types, want 47", len(reservedTypes))
	}
}

// TestTermsMatchC cross-checks the special terms and the image version
// with runtime/vm/term.h and runtime/vm/image.h.
func TestTermsMatchC(t *testing.T) {
	src, err := os.ReadFile("../runtime/vm/term.h")
	if err != nil {
		t.Fatal(err)
	}
	c := map[string]uint64{}
	for _, m := range regexp.MustCompile(`#define\s+(T_\w+)\s+\(\(term\)0x([0-9a-fA-F]+)\)`).FindAllStringSubmatch(string(src), -1) {
		c[m[1]], _ = strconv.ParseUint(m[2], 16, 32)
	}
	want := map[string]uint32{"T_NIL": TermNil, "T_FALSE": TermFalse, "T_TRUE": TermTrue,
		"T_EMPTYSTR": TermEmptyStr, "T_ZEROREF": TermZeroRef, "T_ZEROPID": TermZeroPid,
		"T_NILSLICE": TermNilSlice, "T_EMPTYBYTES": TermEmptyBytes}
	if len(c) != len(want) {
		t.Errorf("runtime/vm/term.h defines %v, want the twins of %v", c, want)
	}
	for n, v := range want {
		if g, ok := c[n]; !ok || uint32(g) != v {
			t.Errorf("%s: C=%#x Go=%#x (present=%v)", n, g, v, ok)
		}
	}
	// Struct flags.
	flags := map[string]uint64{}
	for _, m := range regexp.MustCompile(`#define\s+(STRUCT_(?:MUT|MAP|BOX))\s+0x([0-9a-fA-F]+)u`).FindAllStringSubmatch(string(src), -1) {
		flags[m[1]], _ = strconv.ParseUint(m[2], 16, 32)
	}
	for n, v := range map[string]uint32{"STRUCT_MUT": StructMut, "STRUCT_MAP": StructMap, "STRUCT_BOX": StructBox} {
		if g, ok := flags[n]; !ok || uint32(g) != v {
			t.Errorf("%s: C=%#x Go=%#x (present=%v)", n, g, v, ok)
		}
	}
	// Object kinds.
	kinds := map[string]uint64{}
	for _, m := range regexp.MustCompile(`\b(K_\w+)\s*=\s*(\d+)`).FindAllStringSubmatch(string(src), -1) {
		kinds[m[1]], _ = strconv.ParseUint(m[2], 10, 32)
	}
	wantK := map[string]uint32{"K_STRUCT": KStruct, "K_STRING": KString, "K_INT": KInt,
		"K_CLOSURE": KClosure, "K_REF": KRef, "K_INT64": KInt64, "K_SLICE": KSlice,
		"K_ARRAY": KArray, "K_BYTEARR": KBytearr, "K_BYTES": KBytes, "K_BUF": KBuf,
		"K_FLOAT": KFloat, "K_FORWARD": KForward}
	if len(kinds) != len(wantK) {
		t.Errorf("runtime/vm/term.h defines kinds %v, want the twins of %v", kinds, wantK)
	}
	for n, v := range wantK {
		if g, ok := kinds[n]; !ok || uint32(g) != v {
			t.Errorf("%s: C=%d Go=%d (present=%v)", n, g, v, ok)
		}
	}
	src, err = os.ReadFile("../runtime/vm/image.h")
	if err != nil {
		t.Fatal(err)
	}
	for name, v := range map[string]int{"IMAGE_MAJOR": imageMajor, "IMAGE_MINOR": imageMinor} {
		m := regexp.MustCompile(`#define\s+` + name + `\s+(\d+)u`).FindStringSubmatch(string(src))
		if m == nil || m[1] != strconv.Itoa(v) {
			t.Errorf("%s in runtime/vm/image.h %v, Go %d", name, m, v)
		}
	}
	if newest := max(floatMinor, sizedMinor, ifaceMinor, refMinor); imageMajor != 15 || imageMinor != newest {
		t.Errorf("image format %d.%d, want 15.%d (the newest of floatMinor, sizedMinor, ifaceMinor and refMinor)", imageMajor, imageMinor, newest)
	}
}

// TestConfigMatchC cross-checks the off-heap Bytes defaults with
// runtime/vm/config.h.
func TestConfigMatchC(t *testing.T) {
	src, err := os.ReadFile("../runtime/vm/config.h")
	if err != nil {
		t.Fatal(err)
	}
	c := map[string]int{}
	for _, m := range regexp.MustCompile(`#define\s+(GOGO_BYTES_\w+)\s+(\d+)u`).FindAllStringSubmatch(string(src), -1) {
		c[m[1]], _ = strconv.Atoi(m[2])
	}
	want := map[string]int{"GOGO_BYTES_HANDLES": BytesHandles, "GOGO_BYTES_PER_ACTOR": BytesPerActor,
		"GOGO_BYTES_INLINE": BytesInline}
	if len(c) != len(want) {
		t.Errorf("runtime/vm/config.h defines %v, want the twins of %v", c, want)
	}
	for n, v := range want {
		if g, ok := c[n]; !ok || g != v {
			t.Errorf("%s: C=%d Go=%d (present=%v)", n, g, v, ok)
		}
	}
}

// TestGpioConstants pins the gogo/gpio constants (gogo/port has none).
func TestGpioConstants(t *testing.T) {
	l := testLoader()
	if p, err := l.lib(PortPath); err != nil || len(p.Scope().Names()) == 0 {
		t.Fatalf("gogo/port: %v", err)
	} else {
		for _, name := range p.Scope().Names() {
			if _, ok := p.Scope().Lookup(name).(*types.Const); ok {
				t.Errorf("gogo/port has constant %s", name)
			}
		}
	}
	pkg, err := l.lib(GpioPath)
	if err != nil {
		t.Fatal(err)
	}
	want := map[string]int64{"Input": 0, "InputPullUp": 1, "InputPullDown": 2, "Output": 3,
		"None": 0, "Rising": 1, "Falling": 2, "Both": 3}
	got := map[string]int64{}
	for _, name := range pkg.Scope().Names() {
		if c, ok := pkg.Scope().Lookup(name).(*types.Const); ok {
			got[name], _ = constant.Int64Val(c.Val())
		}
	}
	if len(got) != len(want) {
		t.Errorf("gogo/gpio constants %v, want %v", got, want)
	}
	for n, v := range want {
		if g, ok := got[n]; !ok || g != v {
			t.Errorf("gpio.%s = %d (present=%v), want %d", n, g, ok, v)
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
	want := []uint32{0x4F474F47, 15 << 16, 1, 1, 6, 1, 0, 0}
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
	// v7: the const map follows the code, one bit per const word set at
	// each object start ("main" at 0, "P" at 3).
	if len(w) != 8+3+2+6+1+1+2 || w[len(w)-3] != 1|1<<3 {
		t.Fatalf("const map: image %d words, last %#x", len(w), w[len(w)-1])
	}
}

// The const map takes ceil(consts/32) words and marks K_INT64 constants
// and strings alike, across word boundaries.
func TestImageConstMap(t *testing.T) {
	b := NewImageBuilder()
	f := b.AddFunc("main", 0, 1) // consts 0..2
	b.SetFuncCode(f, []uint32{uint32(OpNop)})
	var starts []uint32
	for i := int64(0); i < 11; i++ { // 11 K_INT64s: 3..35
		starts = append(starts, b.AddInt64(1<<40+i))
	}
	starts = append(starts, b.AddString("abcdefgh")) // 36..39
	raw, err := b.Bytes()
	if err != nil {
		t.Fatal(err)
	}
	w := words(raw)
	if w[4] != 40 {
		t.Fatalf("consts %d words, want 40", w[4])
	}
	cmap := w[len(w)-4 : len(w)-2]
	if len(w) != 8+3+40+1+2+2 {
		t.Fatalf("image %d words", len(w))
	}
	want := [2]uint32{1}
	for _, s := range starts {
		want[s/32] |= 1 << (s % 32)
	}
	if cmap[0] != want[0] || cmap[1] != want[1] {
		t.Fatalf("const map %#x %#x, want %#x %#x", cmap[0], cmap[1], want[0], want[1])
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
	a.Emit(OpClear, 1, 3, 0)
	a.EmitExt(OpLoadk, 0, 0, 0, TermZeroPid)
	a.EmitExt(OpLoadk, 0, 0, 0, TermZeroRef)
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
		"JMPF r3, @0012", "NEWSTRUCT r2, r0, 2, P", "TYPEIS r2, r0, TyInt", "BUILTIN r0, r1, 2, PRINTLN",
		"CLEAR r1, 3", "LOADK r0, zeropid", "LOADK r0, zeroref"} {
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

// TestReservedTypeCount: the one reserved-type count (R2) matches the
// table the compiler emits first in every image.
func TestReservedTypeCount(t *testing.T) {
	if len(reservedTypes) != int(TypeReservedCount) {
		t.Fatalf("len(reservedTypes) = %d, TypeReservedCount = %d", len(reservedTypes), TypeReservedCount)
	}
}

// TestReservedFieldsMatchC: the VM's field counts of the reserved types
// (TYPE_NFIELDS in runtime/vm/opcodes.h, which image_load checks every
// reserved row against) match reservedTypes.
func TestReservedFieldsMatchC(t *testing.T) {
	src, err := os.ReadFile("../runtime/vm/opcodes.h")
	if err != nil {
		t.Fatal(err)
	}
	var c []int
	for _, m := range regexp.MustCompile(`TYPE_NF2\((\d+), (\d+)\),`).FindAllStringSubmatch(string(src), -1) {
		a, _ := strconv.Atoi(m[1])
		b, _ := strconv.Atoi(m[2])
		c = append(c, a, b)
	}
	if len(c) != (len(reservedTypes)+1)/2*2 {
		t.Fatalf("TYPE_NFIELDS has %d entries, reservedTypes %d", len(c), len(reservedTypes))
	}
	for i, rt := range reservedTypes {
		if c[i] != len(rt.fields) {
			t.Errorf("reserved type %d (%s): TYPE_NFIELDS %d, reservedTypes %d", i, rt.name, c[i], len(rt.fields))
		}
		if len(rt.fields) > 15 {
			t.Errorf("reserved type %d (%s): %d fields, TYPE_NFIELDS packs at most 15", i, rt.name, len(rt.fields))
		}
	}
	if len(reservedTypes)%2 == 1 && c[len(c)-1] != 0 {
		t.Errorf("TYPE_NFIELDS pad entry %d, want 0", c[len(c)-1])
	}
}

// TestImageConstAreaLimit: a const ref carries a 17-bit word offset (the
// top bits of the runtime term are the module tag, runtime/vm/term.h), so a const
// area past 1<<17 words is a compile error, not an image the VM rejects.
func TestImageConstAreaLimit(t *testing.T) {
	b := NewImageBuilder()
	f := b.AddFunc("main", 0, 1)
	b.SetFuncCode(f, []uint32{uint32(OpRet)})
	// "main" takes 3 words; the big string 2 + payload: exactly the limit.
	b.AddString(strings.Repeat("x", (MaxConstWords-5)*4))
	if _, err := b.Bytes(); err != nil {
		t.Fatalf("at the limit: %v", err)
	}
	b.AddString("y") // 3 more words
	_, err := b.Bytes()
	if err == nil || !strings.Contains(err.Error(), "const area") {
		t.Fatalf("past the limit: err = %v", err)
	}
}

func TestImageEmptyFuncRejected(t *testing.T) {
	b := NewImageBuilder()
	b.AddFunc("x", 0, 1)
	if _, err := b.Bytes(); err == nil {
		t.Fatal("expected error for func without code")
	}
}

// TestFsMatchC cross-checks gogo/fs with C and gogo/errors: every fs.Err
// constant is the gogo/errors code of the same name (whose GOGO_E_* twin
// tests/errors_test.go checks), and the B_FS op codes and open modes of
// the compiler match runtime/vfs/vfs.h.
func TestFsMatchC(t *testing.T) {
	src, err := os.ReadFile("../runtime/vfs/vfs.h")
	if err != nil {
		t.Fatal(err)
	}
	c := map[string]int64{} // C name -> value
	for _, m := range regexp.MustCompile(`\b((?:VFS|FS)_[A-Z]+)\s*=\s*(\d+)`).FindAllStringSubmatch(string(src), -1) {
		c[m[1]], _ = strconv.ParseInt(m[2], 10, 64)
	}
	l := testLoader()
	pkg, err := l.lib(FsPath)
	if err != nil {
		t.Fatal(err)
	}
	if n := sharedCodes(t, l, pkg, "Err"); n != 14 {
		t.Errorf("fs has %d Err constants, want 14", n)
	}
	for name, cname := range map[string]string{"Read": "VFS_READ", "Write": "VFS_WRITE", "Append": "VFS_APPEND"} {
		k, ok := pkg.Scope().Lookup(name).(*types.Const)
		if !ok {
			t.Errorf("fs.%s missing from the stub", name)
			continue
		}
		v, _ := constant.Int64Val(k.Val())
		if cv, ok := c[cname]; !ok || cv != v {
			t.Errorf("fs.%s = %d, %s = %d (present=%v)", name, v, cname, cv, ok)
		}
	}
	// The op codes of the compiler.
	ops := map[string]int32{"FS_OPEN": FsOpen, "FS_READ": FsRead, "FS_WRITE": FsWrite, "FS_SYNC": FsSync, "FS_SIZE": FsSize,
		"FS_READFILE": FsReadFile, "FS_WRITEFILE": FsWriteFile, "FS_STAT": FsStat, "FS_READDIR": FsReadDir,
		"FS_MKDIR": FsMkdir, "FS_REMOVE": FsRemove, "FS_RENAME": FsRename, "FS_USAGE": FsUsage, "FS_WRITEAT": FsWriteAt, "FS_OPENERR": FsOpenErr}
	for name, v := range ops {
		if cv, ok := c[name]; !ok || cv != int64(v) {
			t.Errorf("%s: C=%d Go=%d (present=%v)", name, cv, v, ok)
		}
	}
	nOps := 0
	for name := range c {
		if strings.HasPrefix(name, "FS_") {
			nOps++
		}
	}
	if nOps != len(ops) {
		t.Errorf("vfs.h has %d FS_ op codes, the compiler %d", nOps, len(ops))
	}
	// Every op code is reached by exactly one fsOps entry.
	seen := map[int32]string{}
	for name, op := range fsOps {
		if prev, dup := seen[op]; dup {
			t.Errorf("op %d used by %s and %s", op, prev, name)
		}
		seen[op] = name
	}
	if len(seen) != len(ops) {
		t.Errorf("fsOps covers %d op codes, want %d", len(seen), len(ops))
	}
}

// TestCodeMatchC cross-checks gogo/code with C and gogo/errors: every
// code.Err constant is the gogo/errors code of the same name, and the
// B_CODE op codes match CODE_OP_* (runtime/vm/module.h).
func TestCodeMatchC(t *testing.T) {
	c := map[string]int64{}
	src, err := os.ReadFile("../runtime/vm/module.h")
	if err != nil {
		t.Fatal(err)
	}
	for _, m := range regexp.MustCompile(`\b(CODE_\w+)\s*=\s*(\d+)`).FindAllStringSubmatch(string(src), -1) {
		c[m[1]], _ = strconv.ParseInt(m[2], 10, 64)
	}
	l := testLoader()
	pkg, err := l.lib(CodePath)
	if err != nil {
		t.Fatal(err)
	}
	if n := sharedCodes(t, l, pkg, "Err"); n != 8 {
		t.Errorf("code has %d Err constants, want 8", n)
	}
	ops := map[string]int32{"CODE_OP_LOAD": CodeLoad, "CODE_OP_SPAWN": CodeSpawn, "CODE_OP_SPAWN_LINK": CodeSpawnLink,
		"CODE_OP_SPAWN_OPT": CodeSpawnOpt, "CODE_OP_HOLD": CodeHold, "CODE_OP_RELEASE": CodeRelease,
		"CODE_OP_MISSING": CodeMissing, "CODE_OP_MEM_USED": CodeMemUsed}
	nOps := 0
	for name := range c {
		if strings.HasPrefix(name, "CODE_OP_") {
			nOps++
		}
	}
	if nOps != len(ops) || len(codeOps) != len(ops) {
		t.Errorf("module.h has %d CODE_OP_ codes, the compiler %d (codeOps %d)", nOps, len(ops), len(codeOps))
	}
	for name, v := range ops {
		if cv, ok := c[name]; !ok || cv != int64(v) {
			t.Errorf("%s: C=%d Go=%d (present=%v)", name, cv, v, ok)
		}
	}
	// Every gogo/code function has an op.
	for _, name := range pkg.Scope().Names() {
		if fn, ok := pkg.Scope().Lookup(name).(*types.Func); ok {
			if _, ok := codeOps["code."+fn.Name()]; !ok {
				t.Errorf("code.%s has no B_CODE op", fn.Name())
			}
		}
	}
}

// sharedCodes checks that every constant of pkg's type typeName equals
// the gogo/errors constant of the same name, and returns how many there
// are.
func sharedCodes(t *testing.T, l *loader, pkg *types.Package, typeName string) int {
	t.Helper()
	ep, err := l.lib(ErrorsPath)
	if err != nil {
		t.Fatal(err)
	}
	// The package's typeName is an alias of errors.Code: its constants
	// of that type are Codes.
	if a, ok := pkg.Scope().Lookup(typeName).(*types.TypeName); !ok || !a.IsAlias() || types.Unalias(a.Type()) != ep.Scope().Lookup("Code").Type() {
		t.Fatalf("%s.%s is not an alias of errors.Code", pkg.Name(), typeName)
	}
	n := 0
	for _, name := range pkg.Scope().Names() {
		k, ok := pkg.Scope().Lookup(name).(*types.Const)
		if !ok {
			continue
		}
		if nt, ok := types.Unalias(k.Type()).(*types.Named); !ok || nt.Obj() != ep.Scope().Lookup("Code") {
			continue
		}
		n++
		v, _ := constant.Int64Val(k.Val())
		e, ok := ep.Scope().Lookup(name).(*types.Const)
		if !ok {
			t.Errorf("%s.%s has no errors.%s", pkg.Name(), name, name)
			continue
		}
		if ev, _ := constant.Int64Val(e.Val()); ev != v {
			t.Errorf("%s.%s = %d, errors.%s = %d", pkg.Name(), name, v, name, ev)
		}
	}
	return n
}

// testLoader is a loader of the embedded libraries, for the tests that
// read a library's declarations (lib).
func testLoader() *loader {
	return &loader{fset: token.NewFileSet(), libs: lib.StubFS, pkgs: map[string]*types.Package{},
		info: &types.Info{Types: map[ast.Expr]types.TypeAndValue{}, Defs: map[*ast.Ident]types.Object{},
			Uses: map[*ast.Ident]types.Object{}, Selections: map[*ast.SelectorExpr]*types.Selection{}, Implicits: map[ast.Node]types.Object{}}}
}

// lib loads library path (and what it imports) and returns it.
func (l *loader) lib(path string) (*types.Package, error) {
	if err := l.loadPkg(path, nil, token.NoPos); err != nil {
		return nil, err
	}
	return l.pkgs[path], nil
}
