package compiler

import (
	"encoding/binary"
	"fmt"
	"go/ast"
	"go/types"
	"regexp"
	"strconv"
	"strings"
	"testing"
)

// compileSrc compiles src and returns its disassembly.
func compileSrc(t *testing.T, src string) string {
	t.Helper()
	prog, err := Load(writeProg(t, src))
	if err != nil {
		t.Fatalf("Load: %v", err)
	}
	img, err := Compile(prog)
	if err != nil {
		t.Fatalf("Compile: %v", err)
	}
	dis, err := Disassemble(img)
	if err != nil {
		t.Fatalf("Disassemble: %v", err)
	}
	return dis
}

// compileErr compiles src and returns the error, failing if there is none.
func compileErr(t *testing.T, src string) error {
	t.Helper()
	prog, err := Load(writeProg(t, src))
	if err != nil {
		t.Fatalf("Load: %v", err)
	}
	img, err := Compile(prog)
	if err == nil {
		t.Fatal("expected a compile error, got none")
	}
	if img != nil {
		t.Error("image returned together with an error")
	}
	return err
}

// funcDis returns the header line and body of function name.
func funcDis(t *testing.T, dis, name string) (header string, body []string) {
	t.Helper()
	lines := strings.Split(dis, "\n")
	for i, l := range lines {
		if strings.HasPrefix(l, "func "+name+" (") {
			for _, b := range lines[i+1:] {
				if !strings.HasPrefix(b, "  ") {
					break
				}
				body = append(body, b)
			}
			return l, body
		}
	}
	t.Fatalf("function %s not found in:\n%s", name, dis)
	return "", nil
}

var insRE = regexp.MustCompile(`^\s+(\d+)\s+(\w+)\s*(.*)$`)

type ins struct {
	pc   int
	op   string
	args string
}

func parseIns(t *testing.T, body []string) []ins {
	t.Helper()
	var out []ins
	for _, l := range body {
		m := insRE.FindStringSubmatch(l)
		if m == nil {
			t.Fatalf("bad disassembly line %q", l)
		}
		pc, _ := strconv.Atoi(m[1])
		out = append(out, ins{pc, m[2], m[3]})
	}
	return out
}

// jumpTarget returns the @NNNN target of a jump instruction.
func jumpTarget(t *testing.T, in ins) int {
	t.Helper()
	i := strings.Index(in.args, "@")
	if i < 0 {
		t.Fatalf("no jump target in %q", in.args)
	}
	n, err := strconv.Atoi(in.args[i+1:])
	if err != nil {
		t.Fatal(err)
	}
	return n
}

func countOp(code []ins, op, argSubstr string) int {
	n := 0
	for _, in := range code {
		if in.op == op && strings.Contains(in.args, argSubstr) {
			n++
		}
	}
	return n
}

const fibSrc = `package main

func fib(n int) int {
	if n < 2 {
		return n
	}
	return fib(n-1) + fib(n-2)
}

func main() {
	println(fib(10))
}
`

func TestCompileFib(t *testing.T) {
	dis := compileSrc(t, fibSrc)
	header, body := funcDis(t, dis, "fib")
	code := parseIns(t, body)
	for _, op := range []string{"LT", "JMPF", "ADD", "RET"} {
		if countOp(code, op, "") == 0 {
			t.Errorf("fib lacks %s:\n%s", op, dis)
		}
	}
	if n := countOp(code, "CALL", "fib"); n != 2 {
		t.Errorf("fib has %d CALL fib, want 2:\n%s", n, dis)
	}
	m := regexp.MustCompile(`params=1 regs=(\d+)`).FindStringSubmatch(header)
	if m == nil {
		t.Fatalf("bad header %q", header)
	}
	if r, _ := strconv.Atoi(m[1]); r > 6 {
		t.Errorf("fib uses %d registers, want <= 6", r)
	}
	if !strings.Contains(dis, "func main (params=0") {
		t.Errorf("no main function:\n%s", dis)
	}
}

func TestCompileStruct(t *testing.T) {
	dis := compileSrc(t, `package main

type P struct{ X, Y int }

func main() {
	p := P{1, 2}
	println(p.Y)
}
`)
	_, body := funcDis(t, dis, "main")
	code := parseIns(t, body)
	if !regexp.MustCompile(`NEWSTRUCT r\d+, r\d+, 2, P\n`).MatchString(dis) {
		t.Errorf("no NEWSTRUCT ... 2, P:\n%s", dis)
	}
	if countOp(code, "GETFIELD", ", 1") != 1 {
		t.Errorf("no GETFIELD ... 1:\n%s", dis)
	}
}

func TestCompileBigConst(t *testing.T) {
	dis := compileSrc(t, `package main

func main() {
	x := 1 << 30
	y := 5
	println(x, y)
}
`)
	if !strings.Contains(dis, "LOADI32 r0, 1073741824") {
		t.Errorf("no LOADI32 for 1<<30:\n%s", dis)
	}
	if !strings.Contains(dis, "LOADK r1, 5") {
		t.Errorf("no LOADK for 5:\n%s", dis)
	}
}

func TestCompileShortCircuit(t *testing.T) {
	dis := compileSrc(t, `package main

func f() bool { return true }

func g(a bool) bool { return a && f() }

func h(a bool) bool { return a || f() }

func main() {
	println(g(false), h(true))
}
`)
	for _, tc := range []struct{ fn, jump string }{{"g", "JMPF"}, {"h", "JMPT"}} {
		_, body := funcDis(t, dis, tc.fn)
		code := parseIns(t, body)
		var jmp *ins
		callPC := -1
		for i := range code {
			if code[i].op == tc.jump && jmp == nil {
				jmp = &code[i]
			}
			if code[i].op == "CALL" {
				callPC = code[i].pc
			}
		}
		if jmp == nil || callPC < 0 {
			t.Fatalf("%s: missing %s or CALL:\n%s", tc.fn, tc.jump, dis)
		}
		if !(jmp.pc < callPC && jumpTarget(t, *jmp) > callPC) {
			t.Errorf("%s: %s at %d does not skip CALL at %d:\n%s", tc.fn, tc.jump, jmp.pc, callPC, dis)
		}
	}
}

func TestCompileErrorsSurface(t *testing.T) {
	err := compileErr(t, "package main\nfunc main() {\n\tm := map[string]int{}\n\t_ = m\n}\n")
	if !strings.Contains(err.Error(), "map") {
		t.Errorf("error %q does not mention map", err)
	}
}

func TestCompileAcceptSrc(t *testing.T) {
	dis := compileSrc(t, acceptSrc)
	for _, want := range []string{"SELF", "SPAWN", "SEND", "RECEIVE", "MEMORY", "MKCLOSURE", "TYPEIS"} {
		if !strings.Contains(dis, want) {
			t.Errorf("no %s in:\n%s", want, dis)
		}
	}
}

func TestCompileTypeSwitch(t *testing.T) {
	dis := compileSrc(t, `package main

import "gogo/actor"

type A struct{ X int }

type B struct{ Y int }

func main() {
	switch m := actor.Receive().(type) {
	case A:
		println(m.X)
	case B:
		println(m.Y)
	default:
		println("other")
	}
}
`)
	_, body := funcDis(t, dis, "main")
	code := parseIns(t, body)
	if n := countOp(code, "BUILTIN", "RECEIVE"); n != 1 {
		t.Errorf("%d BUILTIN RECEIVE, want 1:\n%s", n, dis)
	}
	var typeis []ins
	for _, in := range code {
		if in.op == "TYPEIS" {
			typeis = append(typeis, in)
		}
	}
	if len(typeis) != 2 || !strings.HasSuffix(typeis[0].args, ", A") || !strings.HasSuffix(typeis[1].args, ", B") {
		t.Fatalf("want TYPEIS A then TYPEIS B, got %v:\n%s", typeis, dis)
	}
	// m is bound to the register Receive wrote: both TYPEIS test it and
	// both GETFIELDs read it.
	recv := regexp.MustCompile(`BUILTIN (r\d+),`).FindStringSubmatch(dis)
	for _, in := range typeis {
		if !strings.Contains(in.args, ", "+recv[1]+", ") {
			t.Errorf("TYPEIS %q does not test %s:\n%s", in.args, recv[1], dis)
		}
	}
	if n := countOp(code, "GETFIELD", ", "+recv[1]+", 0"); n != 2 {
		t.Errorf("%d GETFIELD of %s, want 2:\n%s", n, recv[1], dis)
	}
}

func TestCompileSpawnClosure(t *testing.T) {
	dis := compileSrc(t, `package main

import "gogo/actor"

type P struct{ X int }

func main() {
	a, b := 1, P{2}
	actor.Spawn(func() {
		println(b.X, a, b)
	})
	actor.Spawn(func() {
		println("no captures")
	})
	println(actor.Info(actor.Self()).Memory)
}
`)
	// Captures become params, in order of first reference: b, then a.
	header, body := funcDis(t, dis, "main.main.func1")
	if !strings.Contains(header, "params=2") {
		t.Errorf("func1 header %q, want params=2:\n%s", header, dis)
	}
	code := parseIns(t, body)
	if countOp(code, "GETFIELD", "r0, 0") != 1 {
		t.Errorf("func1 does not read b.X from r0:\n%s", dis)
	}
	if header, _ := funcDis(t, dis, "main.main.func2"); !strings.Contains(header, "params=0") {
		t.Errorf("func2 header %q, want params=0", header)
	}
	_, body = funcDis(t, dis, "main")
	code = parseIns(t, body)
	if !regexp.MustCompile(`MKCLOSURE r\d+, r\d+, 2, main\.main\.func1\n`).MatchString(dis) {
		t.Errorf("no MKCLOSURE ... 2, main.main.func1:\n%s", dis)
	}
	if n := countOp(code, "BUILTIN", "SPAWN"); n != 2 {
		t.Errorf("%d BUILTIN SPAWN, want 2:\n%s", n, dis)
	}
	if !regexp.MustCompile(`NEWSTRUCT r\d+, r\d+, 1, ProcInfo\n`).MatchString(dis) {
		t.Errorf("no NEWSTRUCT ProcInfo:\n%s", dis)
	}
}

func TestCompileComparisonsSwap(t *testing.T) {
	dis := compileSrc(t, `package main

func gt(a, b int) bool { return a > b }

func ge(a, b int) bool { return a >= b }

func main() { println(gt(1, 2), ge(1, 2)) }
`)
	_, body := funcDis(t, dis, "gt")
	if countOp(parseIns(t, body), "LT", "r1, r0") != 1 {
		t.Errorf("gt: want LT with swapped operands:\n%s", dis)
	}
	_, body = funcDis(t, dis, "ge")
	if countOp(parseIns(t, body), "LE", "r1, r0") != 1 {
		t.Errorf("ge: want LE with swapped operands:\n%s", dis)
	}
}

func TestCompileConstFoldingOnlyConstants(t *testing.T) {
	dis := compileSrc(t, `package main

const k = 6

func main() {
	a := k * 7
	b := a * 7
	println(a, b)
}
`)
	_, body := funcDis(t, dis, "main")
	code := parseIns(t, body)
	if !strings.Contains(dis, "LOADK r0, 42") {
		t.Errorf("k*7 not folded:\n%s", dis)
	}
	if countOp(code, "MUL", "") != 1 {
		t.Errorf("want exactly one runtime MUL:\n%s", dis)
	}
}

const loopSrc = `package main

func main() {
	sum := 0
	for i := 0; i < 10; i++ {
		for j := 0; j < 10; j++ {
			if j == 3 {
				continue
			}
			if j > 5 {
				break
			}
			sum += j
		}
		if i == 8 {
			break
		}
		sum -= 1
	}
	for sum > 0 {
		sum--
	}
	for {
		break
	}
	println(sum)
}
`

// TestCompileLoops checks break/continue targets of nested loops: every
// jump goes inside the function, and each loop's back edge exists.
func TestCompileLoops(t *testing.T) {
	dis := compileSrc(t, loopSrc)
	_, body := funcDis(t, dis, "main")
	code := parseIns(t, body)
	start, end := code[0].pc, code[len(code)-1].pc
	backEdges := 0
	for _, in := range code {
		if in.op == "JMP" || in.op == "JMPF" || in.op == "JMPT" {
			tgt := jumpTarget(t, in)
			if tgt < start || tgt > end {
				t.Errorf("jump at %d targets %d outside [%d,%d]", in.pc, tgt, start, end)
			}
			if in.op == "JMP" && tgt < in.pc {
				backEdges++
			}
		}
	}
	if backEdges < 4 {
		t.Errorf("want >= 4 back edges (3 loops + continue-free infinite loop), got %d:\n%s", backEdges, dis)
	}
}

func TestCompileImplicitReturn(t *testing.T) {
	dis := compileSrc(t, "package main\nfunc main() {\n\tprintln(1)\n}\n")
	_, body := funcDis(t, dis, "main")
	code := parseIns(t, body)
	n := len(code)
	if n < 2 || code[n-2].op != "LOADK" || !strings.HasSuffix(code[n-2].args, "nil") || code[n-1].op != "RET" {
		t.Errorf("main does not end with LOADK nil; RET:\n%s", dis)
	}
}

func TestCompileAssignForms(t *testing.T) {
	dis := compileSrc(t, `package main

type In struct{ X int }
type Out struct {
	A  In
	S  string
	Ok bool
}

func main() {
	a, b := 1, 2
	a, b = b, a
	a += 3
	b *= a
	a /= 2
	b %= 5
	a -= b
	var z int
	var o Out
	o2 := Out{A: In{X: a}}
	println(a, b, z, o.A.X, o2.A.X, "hi")
}
`)
	for _, want := range []string{"ADD", "MUL", "DIV", "REM", "SUB", `"hi"`, "NEWSTRUCT", "GETFIELD"} {
		if !strings.Contains(dis, want) {
			t.Errorf("missing %s:\n%s", want, dis)
		}
	}
	// a, b = b, a: both values into temporaries first, then the two MOVEs.
	swap := "MOVE r2, r1\n" + "  .*MOVE r3, r0\n" + "  .*MOVE r0, r2\n" + "  .*MOVE r1, r3\n"
	if !regexp.MustCompile(swap).MatchString(dis) {
		t.Errorf("a, b = b, a not compiled as temps then MOVEs:\n%s", dis)
	}
	// Out{} zero value: In{0}, empty string and false fields.
	if !strings.Contains(dis, `LOADK r`) || !strings.Contains(dis, "emptystr") || !strings.Contains(dis, "false") {
		t.Errorf("zero values missing:\n%s", dis)
	}
}

func TestCompileTooManyRegisters(t *testing.T) {
	var sb strings.Builder
	sb.WriteString("package main\nfunc main() {\n")
	for i := 0; i < 300; i++ {
		sb.WriteString("\tv" + strconv.Itoa(i) + " := " + strconv.Itoa(i) + "\n")
	}
	for i := 0; i < 300; i++ {
		sb.WriteString("\t_ = v" + strconv.Itoa(i) + "\n")
	}
	sb.WriteString("}\n")
	err := compileErr(t, sb.String())
	if !strings.Contains(err.Error(), "registers") {
		t.Errorf("error %q does not mention registers", err)
	}
}

// TestCompileKeyedLiteralOrder: keyed values are evaluated in source order,
// not field order.
func TestCompileKeyedLiteralOrder(t *testing.T) {
	dis := compileSrc(t, `package main

type P struct{ X, Y int }

func f(n int) int {
	println(n)
	return n
}

func main() {
	p := P{Y: f(1), X: f(2)}
	println(p.X)
}
`)
	_, body := funcDis(t, dis, "main")
	code := parseIns(t, body)
	var argsSeen []string
	for i, in := range code {
		if in.op == "CALL" && i > 0 && code[i-1].op == "LOADK" {
			argsSeen = append(argsSeen, code[i-1].args[strings.LastIndex(code[i-1].args, " ")+1:])
		}
	}
	if strings.Join(argsSeen, ",") != "1,2" {
		t.Errorf("calls evaluated with args %v, want [1 2]:\n%s", argsSeen, dis)
	}
}

// imageTypes lists the type table of an image as "name/n_fields".
func imageTypes(t *testing.T, img []byte) []string {
	t.Helper()
	w := words(img)
	nf, nt := w[2], w[3]
	types := w[8+nf*3:]
	consts := types[nt*2:]
	var out []string
	for i := uint32(0); i < nt; i++ {
		off := types[i*2+1]
		n := consts[off+1]
		buf := make([]byte, 0, n+4)
		for j := uint32(0); j*4 < n; j++ {
			buf = binary.LittleEndian.AppendUint32(buf, consts[off+2+j])
		}
		out = append(out, string(buf[:n])+"/"+strconv.Itoa(int(types[i*2])))
	}
	return out
}

func compileImage(t *testing.T, src string) []byte {
	t.Helper()
	prog, err := Load(writeProg(t, src))
	if err != nil {
		t.Fatalf("Load: %v", err)
	}
	img, err := Compile(prog)
	if err != nil {
		t.Fatalf("Compile: %v", err)
	}
	return img
}

func TestCompileReservedTypes(t *testing.T) {
	want := []string{"ProcInfo/1", "Reason/2", "ExitMsg/2", "DownMsg/3", "Request/3", "Options/1",
		"gpio.Config/4", "gpio.Write/1", "gpio.Edge/2",
		"net.IfConfig/0", "net.SetAddr/4", "net.Up/3", "net.Down/0", "net.Addr/2", "net.UDP/1",
		"net.SendTo/2", "net.Datagram/2", "net.Dial/3", "net.Listen/1", "net.Shutdown/0",
		"net.Resume/0", "net.Connected/0", "net.Accepted/1",
		"fs.Info/2", "fs.Space/2",
		"kbd.Config/2", "kbd.Key/4", "power.Config/1", "power.Battery/4", "power.Read/0", "code.Module/2",
		"port.Chunk/5", "port.Out/1", "port.Done/2", "port.Closed/2", "imu.Config/3", "imu.Gesture/2", "ir.TX/2", "ir.RX/1",
		"audio.Mic/3", "audio.Speaker/2", "net.DialTLS/4",
		"port.Host/2", "port.Post/3"}
	// A program that names no reserved type carries no reserved row: its
	// own types are numbered from TypeOwnBase.
	img := compileImage(t, "package main\ntype P struct{ X, Y int }\nfunc main() {\n\tvar a any = P{1, 2}\n\tswitch a.(type) {\n\tcase P:\n\t\tprintln(1)\n\t}\n}\n")
	got := imageTypes(t, img)
	if strings.Join(got, " ") != "P/2" {
		t.Fatalf("types %v, want P/2", got)
	}
	if w := words(img); w[7] != 0 {
		t.Errorf("n_reserved %d, want 0", w[7])
	}
	// TYPEIS P names TypeOwnBase, the first own type.
	code := words(img)
	found := false
	for i := 0; i+1 < len(code); i++ {
		if code[i]&0xFF == uint32(OpTypeis) && code[i+1] == TypeOwnBase {
			found = true
		}
	}
	if !found {
		t.Errorf("no TYPEIS with type id %d", TypeOwnBase)
	}
	// Using actor types emits the reserved rows up to the highest used
	// (Reason, 1), at their fixed indices, before the own types; a row
	// the program does not use (ProcInfo) has 0 fields.
	img = compileImage(t, "package main\nimport \"gogo/actor\"\ntype U struct{ A int }\nfunc main() {\n\tu := U{1}\n\tvar r actor.Reason\n\tprintln(u, r)\n}\n")
	if got := imageTypes(t, img); strings.Join(got, " ") != "ProcInfo/0 Reason/2 U/1" {
		t.Fatalf("types %v, want ProcInfo/0 Reason/2 U/1", got)
	}
	if w := words(img); w[7] != 2 {
		t.Errorf("n_reserved %d, want 2", w[7])
	}
	// The last reserved type brings every row, the others with 0 fields.
	img = compileImage(t, "package main\nimport \"gogo/port\"\nfunc main() {\n\tvar p port.Post\n\tprintln(p.Kind)\n}\n")
	var empty []string
	for _, w := range want[:len(want)-1] {
		empty = append(empty, w[:strings.Index(w, "/")]+"/0")
	}
	if got := imageTypes(t, img); strings.Join(got, " ") != strings.Join(append(empty, "port.Post/3"), " ") {
		t.Fatalf("types %v, want %v then port.Post/3", got, empty)
	}
}

// TestImageMinorStamp: an image is stamped with the highest minor among
// what it uses (opcodes, builtins, reserved types), 0 when nothing newer
// than 15.0 is used. A reserved row it carries but does not use counts
// for nothing.
func TestImageMinorStamp(t *testing.T) {
	src := "package main\nimport \"gogo/sys\"\nfunc main() {\n\tprintln(sys.Random())\n}\n"
	if v := words(compileImage(t, src))[1]; v != 15<<16 {
		t.Fatalf("version %#x, want 15.0", v)
	}
	defer saveMinors()()
	builtinMinor[BRandom] = 2
	if v := words(compileImage(t, src))[1]; v != 15<<16|2 {
		t.Errorf("version %s, want 15.2 (B_RANDOM)", VersionString(v))
	}
	clear(builtinMinor)
	typeMinor["Reason"] = 1
	if v := words(compileImage(t, src))[1]; v != 15<<16 {
		t.Errorf("version %s, want 15.0 (no reserved row)", VersionString(v))
	}
	src = "package main\nimport \"gogo/actor\"\nfunc main() {\n\tvar o actor.Options\n\tprintln(o.MaxMemory)\n}\n"
	if v := words(compileImage(t, src))[1]; v != 15<<16 {
		t.Errorf("version %s, want 15.0 (the Reason row comes with Options, unused)", VersionString(v))
	}
	typeMinor["Options"] = 1
	if v := words(compileImage(t, src))[1]; v != 15<<16|1 {
		t.Errorf("version %s, want 15.1 (Options used)", VersionString(v))
	}
	clear(typeMinor)
	// The review's case: a program that uses only port.Done, after a
	// pretend field was appended to a lower reserved type (kbd.Config).
	done := "package main\nimport (\n\t\"gogo/actor\"\n\t\"gogo/port\"\n)\nfunc main() {\n\tswitch m := actor.Receive().(type) {\n\tcase port.Done:\n\t\tprintln(m.Seq)\n\t}\n}\n"
	typeMinor["kbd.Config"] = 1
	img := compileImage(t, done)
	if v := words(img)[1]; v != 15<<16 {
		t.Errorf("version %s, want 15.0 (kbd.Config not used)", VersionString(v))
	}
	if got := imageTypes(t, img); got[TypeKbdConfig] != "kbd.Config/0" || got[TypeDone] != "port.Done/2" {
		t.Errorf("rows %v: want kbd.Config/0 and port.Done/2", got)
	}
	typeMinor["port.Done"] = 2
	if v := words(compileImage(t, done))[1]; v != 15<<16|2 {
		t.Errorf("version %s, want 15.2 (port.Done used)", VersionString(v))
	}
	clear(typeMinor)
	opMinor[OpGetfield] = 3
	if v := words(compileImage(t, src))[1]; v != 15<<16|3 {
		t.Errorf("version %s, want 15.3 (GETFIELD)", VersionString(v))
	}
}

// TestScreenConfigMinor: screen.Config is reserved type 44 of 15.1; a
// program that uses it is stamped 15.1, one that does not stays 15.0.
func TestScreenConfigMinor(t *testing.T) {
	use := "package main\nimport (\n\t\"gogo/port\"\n\t\"gogo/screen\"\n)\nfunc main() {\n\t_, _ = port.Open(\"screen\", screen.Config{})\n}\n"
	img := compileImage(t, use)
	if v := words(img)[1]; v != 15<<16|1 {
		t.Errorf("version %s, want 15.1", VersionString(v))
	}
	if got := imageTypes(t, img); len(got) < 45 || got[TypeScreenConfig] != "screen.Config/2" {
		t.Errorf("rows %v: want screen.Config/2 at %d", got, TypeScreenConfig)
	}
	other := "package main\nimport \"gogo/screen\"\nfunc main() {\n\tprintln(screen.OpText)\n}\n"
	if v := words(compileImage(t, other))[1]; v != 15<<16 {
		t.Errorf("version %s, want 15.0 (constants only)", VersionString(v))
	}
}

func TestCheckVersion(t *testing.T) {
	for _, v := range []uint32{15 << 16, 15<<16 | 1} {
		if err := CheckVersion(v); err != nil {
			t.Errorf("%s: %v", VersionString(v), err)
		}
	}
	for _, v := range []uint32{14, 15, 14 << 16, 15<<16 | 2, 16 << 16} {
		if err := CheckVersion(v); err == nil {
			t.Errorf("%s accepted", VersionString(v))
		}
	}
}

func TestCompileZeroStringIsEmptyStr(t *testing.T) {
	dis := compileSrc(t, "package main\nimport \"gogo/actor\"\nfunc main() {\n\tvar s string\n\tvar r actor.Reason\n\tprintln(s, r)\n}\n")
	if n := strings.Count(dis, "LOADK r"); n < 2 || strings.Count(dis, ", emptystr") != 2 {
		t.Errorf("want two LOADK emptystr:\n%s", dis)
	}
}

func TestCompileTwoResultBuiltins(t *testing.T) {
	dis := compileSrc(t, `package main

import "gogo/actor"

func main() {
	x := 1
	m, ok := actor.ReceiveTimeout(x)
	var p actor.PID
	var found bool
	p, found = actor.Whereis("a")
	println(m, ok, x, p, found)
}
`)
	_, body := funcDis(t, dis, "main")
	code := parseIns(t, body)
	// Locals: x=r0, m=r1, ok=r2, p=r3, found=r4. Results land in fresh
	// temporaries above the locals and are then moved.
	check := func(name string, first int, targets [2]int) {
		t.Helper()
		for i, in := range code {
			if in.op != "BUILTIN" || !strings.HasSuffix(in.args, ", "+name) {
				continue
			}
			ra, _ := strconv.Atoi(regexp.MustCompile(`^r(\d+),`).FindStringSubmatch(in.args)[1])
			if ra < first {
				t.Errorf("%s writes r%d and r%d, clobbering locals below r%d:\n%s", name, ra, ra+1, first, dis)
			}
			if i+2 >= len(code) ||
				code[i+1].args != "r"+itoa(targets[0])+", r"+itoa(ra) ||
				code[i+2].args != "r"+itoa(targets[1])+", r"+itoa(ra+1) {
				t.Errorf("%s results not moved to r%d, r%d:\n%s", name, targets[0], targets[1], dis)
			}
			return
		}
		t.Errorf("no BUILTIN %s:\n%s", name, dis)
	}
	check("RECEIVE_TIMEOUT", 3, [2]int{1, 2})
	check("WHEREIS", 5, [2]int{3, 4})
}

// TestCompileTwoResultTargets covers target shapes of a, b := / = with a
// two-result builtin: the same variable twice, blanks, partial
// redeclaration and a new inner variable whose initializer reads the outer
// one of the same name.
func TestCompileTwoResultTargets(t *testing.T) {
	dis := compileSrc(t, `package main

import "gogo/actor"

func main() {
	var x any
	x, x = actor.ReceiveTimeout(1)
	_, _ = actor.ReceiveTimeout(2)
	p := actor.Self()
	p, ok := actor.Whereis("a")
	{
		p, ok := actor.Call(p, 3, 4)
		println(p, ok)
	}
	println(x, p, ok)
}
`)
	_, body := funcDis(t, dis, "main")
	code := parseIns(t, body)
	// Locals: x=r0, p=r1, ok=r2; the inner p=r3, ok=r4.
	type want struct {
		name    string
		first   int      // lowest register the results may use
		moves   []string // MOVE destinations right after, in order
		argFrom string   // a MOVE/arg source that must appear before it, or ""
	}
	cases := []want{
		{"RECEIVE_TIMEOUT", 1, []string{"r0", "r0"}, ""},
		{"WHEREIS", 3, []string{"r1", "r2"}, ""},
		{"CALL", 5, []string{"r3", "r4"}, "r1"},
	}
	seen := map[string]int{}
	for i, in := range code {
		if in.op != "BUILTIN" {
			continue
		}
		for _, c := range cases {
			if !strings.HasSuffix(in.args, ", "+c.name) {
				continue
			}
			seen[c.name]++
			if c.name == "RECEIVE_TIMEOUT" && seen[c.name] == 2 {
				// _, _ = ...: no moves.
				if i+1 < len(code) && code[i+1].op == "MOVE" {
					t.Errorf("blank targets got a MOVE:\n%s", dis)
				}
				continue
			}
			m := regexp.MustCompile(`^r(\d+), r(\d+), (\d+),`).FindStringSubmatch(in.args)
			ra, _ := strconv.Atoi(m[1])
			if ra < c.first {
				t.Errorf("%s writes r%d/r%d below r%d:\n%s", c.name, ra, ra+1, c.first, dis)
			}
			for j, dst := range c.moves {
				if i+1+j >= len(code) || code[i+1+j].args != dst+", r"+itoa(ra+j) {
					t.Errorf("%s result %d not moved to %s:\n%s", c.name, j, dst, dis)
				}
			}
			if c.argFrom != "" {
				// The call's first argument is the outer p (r1), copied into
				// the argument register before the BUILTIN.
				base, _ := strconv.Atoi(m[2])
				found := false
				for _, prev := range code[:i] {
					if prev.op == "MOVE" && prev.args == "r"+itoa(base)+", "+c.argFrom {
						found = true
					}
				}
				if !found {
					t.Errorf("%s does not read the outer p from %s:\n%s", c.name, c.argFrom, dis)
				}
			}
		}
	}
	if seen["RECEIVE_TIMEOUT"] != 2 || seen["WHEREIS"] != 1 || seen["CALL"] != 1 {
		t.Errorf("builtins seen %v:\n%s", seen, dis)
	}
}

func TestCompileActorBuiltins(t *testing.T) {
	calls := map[string]string{
		"actor.SpawnLink(func() {})":                              "SPAWN_LINK",
		"actor.SpawnOpt(func() {}, actor.Options{})":              "SPAWN_OPT",
		"actor.Link(actor.Self())":                                "LINK",
		"actor.Unlink(actor.Self())":                              "UNLINK",
		"actor.TrapExit(true)":                                    "TRAP_EXIT",
		"actor.Exit(actor.Normal)":                                "EXIT",
		"actor.Kill(actor.Self(), 1)":                             "KILL",
		"println(actor.Monitor(actor.Self()))":                    "MONITOR",
		"actor.Demonitor(actor.MakeRef())":                        "DEMONITOR",
		"println(actor.MakeRef())":                                "MAKE_REF",
		"println(actor.SendAfter(1, actor.Self(), 2))":            "SEND_AFTER",
		"println(actor.CancelTimer(actor.MakeRef()))":             "CANCEL_TIMER",
		"actor.Sleep(3)":                                          "SLEEP",
		"a, b := actor.Call(actor.Self(), 1, 2)\n\tprintln(a, b)": "CALL",
		"actor.Reply(actor.Request{}, 1)":                         "REPLY",
		"println(actor.Register(\"n\", actor.Self()))":            "REGISTER",
		"a, b := actor.Whereis(\"n\")\n\tprintln(a, b)":           "WHEREIS",
		"actor.Unregister(\"n\")":                                 "UNREGISTER",
	}
	for call, name := range calls {
		dis := compileSrc(t, "package main\nimport \"gogo/actor\"\nfunc main() {\n\t"+call+"\n}\n")
		re := regexp.MustCompile(`BUILTIN r\d+, r\d+, \d+, ` + name + `\n`)
		if !re.MatchString(dis) {
			t.Errorf("%s: no BUILTIN %s:\n%s", call, name, dis)
		}
	}
	// SpawnOpt passes the closure and the options in consecutive registers.
	dis := compileSrc(t, "package main\nimport \"gogo/actor\"\nfunc main() {\n\tx := 1\n\tactor.SpawnOpt(func() { println(x) }, actor.Options{MaxMemory: 64})\n}\n")
	m := regexp.MustCompile(`MKCLOSURE (r\d+), r\d+, 1, main\.main\.func1\n(?s:.*)NEWSTRUCT r(\d+), r\d+, 1, Options\n(?s:.*)BUILTIN r\d+, (r\d+), 2, SPAWN_OPT\n`).FindStringSubmatch(dis)
	if m == nil {
		t.Fatalf("SpawnOpt shape:\n%s", dis)
	}
	clo, _ := strconv.Atoi(m[1][1:])
	opts, _ := strconv.Atoi(m[2])
	if m[1] != m[3] || opts != clo+1 {
		t.Errorf("closure %s and options r%d not consecutive at the BUILTIN base %s:\n%s", m[1], opts, m[3], dis)
	}
}

// A two-result builtin compiled into a single destination register
// (every multi-value context goes through multiCallInto, which passes
// two) is an internal error in codegen, not a write to dst+1.
func TestCodegenGuardsTwoResultBuiltins(t *testing.T) {
	prog, err := Load(writeProg(t, "package main\nimport \"gogo/actor\"\nfunc main() {\n\tactor.ReceiveTimeout(1)\n}\n"))
	if err != nil {
		t.Fatalf("Load: %v", err)
	}
	g := &gen{p: prog, b: NewImageBuilder(), funcIdx: map[*types.Func]uint32{}, typeIdx: map[*types.TypeName]uint32{}}
	if err := g.collectTypes(); err != nil {
		t.Fatal(err)
	}
	var fd *ast.FuncDecl
	for _, d := range prog.Files[0].Decls {
		if f, ok := d.(*ast.FuncDecl); ok {
			fd = f
		}
	}
	call := fd.Body.List[0].(*ast.ExprStmt).X.(*ast.CallExpr)
	f := &fn{g: g, info: &funcInfo{name: "main"}, locals: map[*types.Var]uint8{}, named: -1,
		labelDirty: map[Label]int{}, boundAt: -1, lastClear: -1}
	defer func() {
		ge, ok := recover().(genError)
		if !ok || !strings.Contains(ge.err.Error(), "internal error") || !strings.Contains(ge.err.Error(), "ReceiveTimeout") {
			t.Fatalf("want an internal error naming ReceiveTimeout, got %v", ge.err)
		}
	}()
	f.exprInto(call, f.alloc(call))
}

// A two-result builtin as a call statement (rejected by the checker)
// still gets two destination registers from codegen.
func TestCodegenTwoResultBuiltinStmt(t *testing.T) {
	prog, err := Load(writeProg(t, "package main\nimport \"gogo/actor\"\nfunc main() {\n\tactor.ReceiveTimeout(1)\n}\n"))
	if err != nil {
		t.Fatalf("Load: %v", err)
	}
	g := &gen{p: prog, b: NewImageBuilder(), funcIdx: map[*types.Func]uint32{}, typeIdx: map[*types.TypeName]uint32{}}
	if err := g.collectTypes(); err != nil {
		t.Fatal(err)
	}
	var fd *ast.FuncDecl
	for _, d := range prog.Files[0].Decls {
		if f, ok := d.(*ast.FuncDecl); ok {
			fd = f
		}
	}
	g.nameLits([]*ast.FuncDecl{fd})
	info := &funcInfo{name: "main"}
	if err := g.compileFunc(info, nil, nil, fd.Body); err != nil {
		t.Fatal(err)
	}
	found := false
	for pc := 0; pc < len(info.code); pc++ {
		w := info.code[pc]
		if uint8(w) == OpBuiltin && info.code[pc+1] == BReceiveTimeout {
			found = true
			// dst, dst+1 below the arguments, which start at rb
			if dst, rb := int(w>>8&0xFF), int(w>>16&0xFF); dst+2 > rb {
				t.Errorf("BUILTIN r%d, r%d: want two destination registers below the arguments", dst, rb)
			}
		}
		if OpHasExt(uint8(w)) {
			pc++
		}
	}
	if !found {
		t.Error("no RECEIVE_TIMEOUT")
	}
}

// constObj returns the const-area object a const-ref term names.
func constObj(t *testing.T, img []byte, ref uint32) []uint32 {
	t.Helper()
	w := words(img)
	nf, nt, nc := w[2], w[3], w[4]
	consts := w[8+nf*3+nt*2 : 8+nf*3+nt*2+nc]
	if ref&0xF != 0xA {
		t.Fatalf("0x%08x is not a const ref", ref)
	}
	off := ref >> 4
	if off >= nc {
		t.Fatalf("const ref 0x%08x out of range", ref)
	}
	n := consts[off] >> 6
	return consts[off : off+n]
}

// loadkConsts returns the objects of every LOADK const-ref operand in img.
func loadkConsts(t *testing.T, img []byte) [][]uint32 {
	t.Helper()
	w := words(img)
	nf, nt, nc, ncode := w[2], w[3], w[4], w[5]
	code := w[8+nf*3+nt*2+nc : 8+nf*3+nt*2+nc+ncode]
	var out [][]uint32
	for pc := 0; pc < len(code); pc++ {
		op := uint8(code[pc])
		if op == OpLoadk && code[pc+1]&0xF == 0xA {
			out = append(out, constObj(t, img, code[pc+1]))
		}
		if OpHasExt(op) {
			pc++
		}
	}
	return out
}

func TestCompileInt64Const(t *testing.T) {
	img := compileImage(t, `package main

func main() {
	var a int64 = 1 << 40
	b := int64(-5)
	var c int64
	c++
	println(a, b, c)
}
`)
	// Only values outside 31 bits are const-area boxes; small ones are
	// LOADI32 immediates.
	want := map[int64]bool{1 << 40: false}
	for _, o := range loadkConsts(t, img) {
		if o[0]&63 != KInt64 {
			continue
		}
		if o[0]>>6 != 3 || len(o) != 3 {
			t.Fatalf("K_INT64 const %v is not 3 words", o)
		}
		v := int64(uint64(o[1]) | uint64(o[2])<<32)
		if _, ok := want[v]; ok {
			want[v] = true
		}
	}
	for v, seen := range want {
		if !seen {
			t.Errorf("no LOADK of a K_INT64 const %d", v)
		}
	}
	dis, err := Disassemble(img)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(dis, "int64(1099511627776)") {
		t.Errorf("disassembly does not render the int64 const:\n%s", dis)
	}
	_, body := funcDis(t, dis, "main")
	code := parseIns(t, body)
	for _, v := range []string{"-5", "0", "1"} {
		if countOp(code, "LOADI32", ", "+v) == 0 {
			t.Errorf("no LOADI32 of the small int64 %s:\n%s", v, dis)
		}
	}
	if countOp(code, "ADD64", "") != 1 {
		t.Errorf("c++ on an int64 should be ADD64:\n%s", dis)
	}
}

func TestCompileInt64TypedOps(t *testing.T) {
	dis := compileSrc(t, `package main

type D int64

func sub(a, b int64) int64 { return a - b }

func main() {
	a, b := int64(3), int64(4)
	c := a + b
	println(c*a, c/a, c%a, -c, sub(a, b))
	println(a == b, a != b, a < b, a <= b, a > b, a >= b)
	x, y := 3, 4
	println(x+y, x < y, -x)
	var d D = 5
	d += 2
	println(d+d, d < 9)
	a -= 1
	a++
}
`)
	_, body := funcDis(t, dis, "main")
	code := parseIns(t, body)
	for _, op := range []string{"ADD64", "MUL64", "DIV64", "REM64", "NEG64", "EQ64", "NE64", "LT64", "LE64"} {
		if countOp(code, op, "") == 0 {
			t.Errorf("main lacks %s:\n%s", op, dis)
		}
	}
	// a+b, d+=, d+d, a++; GT/GE swap to LT64/LE64.
	if n := countOp(code, "ADD64", ""); n != 4 {
		t.Errorf("%d ADD64, want 4:\n%s", n, dis)
	}
	if countOp(code, "LT64", "") != 3 || countOp(code, "LE64", "") != 2 {
		t.Errorf("want 3 LT64 (<, >, d<9) and 2 LE64 (<=, >=):\n%s", dis)
	}
	if countOp(code, "SUB64", "") != 1 {
		t.Errorf("a -= 1 should be SUB64:\n%s", dis)
	}
	// The int ops stay generic.
	if countOp(code, "ADD", "") != 1 || countOp(code, "LT", "") != 1 || countOp(code, "NEG", "") != 1 {
		t.Errorf("int ops should stay ADD/LT/NEG:\n%s", dis)
	}
	_, body = funcDis(t, dis, "sub")
	if countOp(parseIns(t, body), "SUB64", "") != 1 {
		t.Errorf("sub lacks SUB64:\n%s", dis)
	}
	if strings.Contains(dis, "BOX64") {
		t.Errorf("no int64 flows into an interface here:\n%s", dis)
	}
}

func TestCompileInt64BoxIntoAny(t *testing.T) {
	dis := compileSrc(t, `package main

import "gogo/actor"

type S struct {
	V any
	N int64
}

func take(v any)       {}
func give(x int64) any { return x }
func keep(x int64) int64 { return x }

func main() {
	i64 := int64(5)
	var x any = i64
	x = i64
	take(i64)
	s := S{i64, i64}
	s = S{V: i64}
	actor.Send(actor.Self(), i64)
	actor.SendAfter(1, actor.Self(), i64)
	r, _ := actor.Call(actor.Self(), i64, 0)
	println(x, s.N, r, i64, keep(i64))
	var c any = int64(7)
	take(c)
}
`)
	_, body := funcDis(t, dis, "main")
	code := parseIns(t, body)
	// var x, x =, take, S{i64, ...}, S{V: ...}, Send, SendAfter, Call;
	// not println, the int64 field N, keep's argument or closures.
	if n := countOp(code, "BOX64", ""); n != 8 {
		t.Errorf("%d BOX64 in main, want 8:\n%s", n, dis)
	}
	_, body = funcDis(t, dis, "give")
	gc := parseIns(t, body)
	if countOp(gc, "BOX64", "") != 1 || gc[len(gc)-1].op != "RET" {
		t.Errorf("give should BOX64 its result before RET:\n%s", dis)
	}
	_, body = funcDis(t, dis, "keep")
	if countOp(parseIns(t, body), "BOX64", "") != 0 {
		t.Errorf("keep returns an int64, not an interface:\n%s", dis)
	}
	// A constant int64 into any is a const-area box: no BOX64, no LOADI32.
	if !strings.Contains(dis, "LOADK r") || !strings.Contains(dis, "int64(7)") {
		t.Errorf("var c any = int64(7) should LOADK a const box:\n%s", dis)
	}
}

func TestCompileInt64Conv(t *testing.T) {
	dis := compileSrc(t, `package main

func main() {
	x := 3
	y := int64(x)
	z := int(y)
	w := int64(y)
	println(z, w)
}
`)
	_, body := funcDis(t, dis, "main")
	code := parseIns(t, body)
	if countOp(code, "CONV", "r1, r0, 1") != 1 || countOp(code, "CONV", "r2, r1, 0") != 1 {
		t.Errorf("want CONV r1, r0, 1 and CONV r2, r1, 0:\n%s", dis)
	}
	if countOp(code, "CONV", "") != 2 {
		t.Errorf("identity conversion should not emit CONV:\n%s", dis)
	}
	dis = compileSrc(t, "package main\nfunc main() {\n\tvar v any = int64(1)\n\tswitch v.(type) {\n\tcase int64:\n\t\tprintln(1)\n\t}\n}\n")
	if !strings.Contains(dis, "TYPEIS r") || !strings.Contains(dis, ", TyInt64") {
		t.Errorf("want TYPEIS ... TyInt64:\n%s", dis)
	}
}

func TestCompileFuncValues(t *testing.T) {
	dis := compileSrc(t, `package main

import "gogo/actor"

type Job struct{ Run func() }

func work() { println("work") }

func main() {
	x := 7
	f := func() { println(x) }
	j := Job{work}
	var z func()
	actor.Spawn(f)
	actor.Spawn(j.Run)
	actor.Spawn(work)
	actor.Spawn(z)
}
`)
	// A top-level function as a value is a closure without captures, and
	// the function itself takes no parameters (nparams == ncaps).
	if n := regexp.MustCompile(`MKCLOSURE r\d+, r\d+, 0, work\n`).FindAllString(dis, -1); len(n) != 2 {
		t.Errorf("%d MKCLOSURE ..., 0, work, want 2:\n%s", len(n), dis)
	}
	if header, _ := funcDis(t, dis, "work"); !strings.Contains(header, "params=0") {
		t.Errorf("work header %q, want params=0", header)
	}
	if !regexp.MustCompile(`MKCLOSURE r\d+, r\d+, 1, main\.main\.func1\n`).MatchString(dis) {
		t.Errorf("no MKCLOSURE ..., 1, main.main.func1:\n%s", dis)
	}
	if header, _ := funcDis(t, dis, "main.main.func1"); !strings.Contains(header, "params=1") {
		t.Errorf("func1 header %q, want params=1", header)
	}
	_, body := funcDis(t, dis, "main")
	if n := countOp(parseIns(t, body), "BUILTIN", "SPAWN"); n != 4 {
		t.Errorf("%d BUILTIN SPAWN, want 4:\n%s", n, dis)
	}
}

func TestCompileLibrary(t *testing.T) {
	libs := fakeLibs(t, map[string]string{"lib/lib.gg": `package lib

import "gogo/actor"

type Spec struct{ N int }

func helper(s Spec) actor.Options { return actor.Options{MaxMemory: s.N} }

func Go(f func(), s Spec) actor.PID {
	return actor.SpawnOpt(func() {
		actor.Spawn(f)
	}, helper(s))
}

func Idle() {}
`})
	// main does not import gogo/actor itself: the reserved types still come
	// first and actor.Options keeps its index.
	src := "package main\nimport \"gogo/lib\"\ntype P struct{ A int }\nfunc main() {\n\tprintln(P{1})\n\tlib.Go(lib.Idle, lib.Spec{4096})\n}\n"
	prog, err := LoadWith(writeProg(t, src), libs)
	if err != nil {
		t.Fatalf("Load: %v", err)
	}
	img, err := Compile(prog)
	if err != nil {
		t.Fatalf("Compile: %v", err)
	}
	want := "ProcInfo/0 Reason/0 ExitMsg/0 DownMsg/0 Request/0 Options/1"
	if got := strings.Join(imageTypes(t, img), " "); !strings.HasPrefix(got, want+" ") || !strings.Contains(got, "lib.Spec/1") || !strings.Contains(got, "P/1") {
		t.Fatalf("types %s, want %s then P/1 and lib.Spec/1", got, want)
	}
	dis, err := Disassemble(img)
	if err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"main", "lib.Go", "lib.helper", "lib.Idle", "lib.Go.func1"} {
		if header, _ := funcDis(t, dis, name); header == "" {
			t.Errorf("no function %s:\n%s", name, dis)
		}
	}
	if !regexp.MustCompile(`CALL r\d+, r\d+, 2, lib\.Go\n`).MatchString(dis) {
		t.Errorf("no CALL lib.Go:\n%s", dis)
	}
	if !regexp.MustCompile(`MKCLOSURE r\d+, r\d+, 0, lib\.Idle\n`).MatchString(dis) {
		t.Errorf("no MKCLOSURE ..., 0, lib.Idle:\n%s", dis)
	}
	if !regexp.MustCompile(`MKCLOSURE r\d+, r\d+, 1, lib\.Go\.func1\n`).MatchString(dis) {
		t.Errorf("no MKCLOSURE ..., 1, lib.Go.func1:\n%s", dis)
	}
	if !regexp.MustCompile(`NEWSTRUCT r\d+, r\d+, 1, Options\n`).MatchString(dis) {
		t.Errorf("no NEWSTRUCT Options:\n%s", dis)
	}
}

// sys.Config is a two-result builtin, usable from libraries.
func TestCompileConfig(t *testing.T) {
	src := `package main

import (
	"gogo/strings"
	"gogo/sys"
)

func main() {
	v, ok := sys.Config("net.ip")
	println(v, ok, strings.HasPrefix(v, "10."))
}
`
	img := compileImage(t, src)
	dis, err := Disassemble(img)
	if err != nil {
		t.Fatal(err)
	}
	if !regexp.MustCompile(`BUILTIN r\d+, r\d+, 1, CONFIG_GET\n`).MatchString(dis) {
		t.Errorf("no BUILTIN CONFIG_GET:\n%s", dis)
	}
}

// gogo/net: its reserved struct types and net.Subscribe.
func TestCompileNet(t *testing.T) {
	src := `package main

import (
	"gogo/actor"
	"gogo/bytes"
	"gogo/net"
	"gogo/port"
)

func main() {
	ok := net.Subscribe()
	u, _ := port.Open("udp", net.UDP{Port: 7})
	actor.Send(u, net.SendTo{Addr: net.Addr{IP: 10<<24 | 1, Port: 9}, Data: bytes.FromString("hi")})
	switch m := actor.Receive().(type) {
	case net.Datagram:
		println(m.From.IP, m.From.Port, m.Data.Len(), ok)
	case net.Up:
		println(m.IP)
	case net.Down:
		println("down")
	}
}
`
	img := compileImage(t, src)
	dis, err := Disassemble(img)
	if err != nil {
		t.Fatal(err)
	}
	for _, re := range []string{
		`BUILTIN r\d+, r\d+, 0, NET_SUBSCRIBE\n`,
		`NEWSTRUCT r\d+, r\d+, 1, net\.UDP\n`,
		`NEWSTRUCT r\d+, r\d+, 2, net\.Addr\n`,
		`NEWSTRUCT r\d+, r\d+, 2, net\.SendTo\n`,
		`TYPEIS r\d+, r\d+, net\.Datagram\n`,
		`TYPEIS r\d+, r\d+, net\.Up\n`,
		`TYPEIS r\d+, r\d+, net\.Down\n`,
	} {
		if !regexp.MustCompile(re).MatchString(dis) {
			t.Errorf("no match for %s:\n%s", re, dis)
		}
	}
	code := words(img)
	found := false
	for i := 0; i+1 < len(code); i++ {
		if code[i]&0xFF == uint32(OpTypeis) && code[i+1] == TypeNetDatagram {
			found = true
		}
	}
	if !found {
		t.Error("no TYPEIS with TYPE_NET_DATAGRAM")
	}
}

func TestCompilePort(t *testing.T) {
	src := `package main

import (
	"gogo/actor"
	"gogo/gpio"
	"gogo/port"
)

func main() {
	p, ok := port.Open("gpio", gpio.Config{Pin: 25, Mode: gpio.Output, Edge: gpio.None, DebounceMs: 20})
	if !ok {
		return
	}
	var q actor.PID
	q, ok = port.Open("gpio", gpio.Config{Pin: 0, Mode: gpio.InputPullUp, Edge: gpio.Both})
	actor.Send(p, gpio.Write{Value: 1})
	switch m := actor.Receive().(type) {
	case gpio.Edge:
		println(m.Pin, m.Value, ok)
	}
	port.Close(p)
	actor.Kill(q, actor.Killed)
}
`
	img := compileImage(t, src)
	want := "ProcInfo/0 Reason/0 ExitMsg/0 DownMsg/0 Request/0 Options/0 gpio.Config/4 gpio.Write/1 gpio.Edge/2"
	if got := strings.Join(imageTypes(t, img), " "); got != want {
		t.Fatalf("types %s, want %s", got, want)
	}
	dis, err := Disassemble(img)
	if err != nil {
		t.Fatal(err)
	}
	for _, re := range []string{
		`NEWSTRUCT r\d+, r\d+, 4, gpio\.Config\n`,
		`BUILTIN r\d+, r\d+, 2, PORT_OPEN\n`,
		`NEWSTRUCT r\d+, r\d+, 1, gpio\.Write\n`,
		`TYPEIS r\d+, r\d+, gpio\.Edge\n`,
		`BUILTIN r\d+, r\d+, 1, PORT_CLOSE\n`,
	} {
		if !regexp.MustCompile(re).MatchString(dis) {
			t.Errorf("no match for %s:\n%s", re, dis)
		}
	}
	if strings.Count(dis, "PORT_OPEN") != 2 {
		t.Errorf("want two PORT_OPEN:\n%s", dis)
	}
	// The literal is NEWSTRUCT with type id 6 (TYPE_GPIO_CONFIG).
	code := words(img)
	found := false
	for i := 0; i+1 < len(code); i++ {
		if code[i]&0xFF == uint32(OpNewstruct) && code[i]>>24 == 4 && code[i+1] == TypeGpioConfig {
			found = true
		}
	}
	if !found || TypeGpioConfig != 6 {
		t.Error("no NEWSTRUCT with type id 6")
	}
}

// clearCovers reports whether some CLEAR at index > from (and before index
// to, if to >= 0) covers register r.
func clearCovers(t *testing.T, code []ins, from, to, r int) bool {
	t.Helper()
	for i := from + 1; i < len(code) && (to < 0 || i < to); i++ {
		if code[i].op != "CLEAR" {
			continue
		}
		m := regexp.MustCompile(`^r(\d+), (\d+)$`).FindStringSubmatch(code[i].args)
		if m == nil {
			t.Fatalf("bad CLEAR operands %q", code[i].args)
		}
		a, _ := strconv.Atoi(m[1])
		n, _ := strconv.Atoi(m[2])
		if a <= r && r < a+n {
			return true
		}
	}
	return false
}

// callDst returns the index of the first CALL of fn at or after start and
// its destination register.
func callDst(t *testing.T, code []ins, start int, fn string) (int, int) {
	t.Helper()
	for i := start; i < len(code); i++ {
		if code[i].op == "CALL" && strings.HasSuffix(code[i].args, ", "+fn) {
			r, _ := strconv.Atoi(regexp.MustCompile(`^r(\d+),`).FindStringSubmatch(code[i].args)[1])
			return i, r
		}
	}
	t.Fatalf("no CALL %s", fn)
	return 0, 0
}

// TestCompileClearsDeadRegisters: registers that held values and become
// dead are cleared (CLEAR) so they stop being GC roots.
func TestCompileClearsDeadRegisters(t *testing.T) {
	dis := compileSrc(t, `package main

func g() any {
	var x any
	return x
}

func exprStmt() {
	g()
	println("x")
}

func blank() {
	_ = g()
	println("x")
}

func block() {
	{
		a := g()
		b := g()
		_, _ = a, b
	}
	println("x")
}

func loop() {
	for i := 0; i < 3; i++ {
		c := g()
		_ = c
	}
}

func main() {
	exprStmt()
	blank()
	block()
	loop()
}
`)
	for _, fn := range []string{"exprStmt", "blank"} {
		_, body := funcDis(t, dis, fn)
		code := parseIns(t, body)
		i, r := callDst(t, code, 0, "g")
		if i+1 >= len(code) || code[i+1].op != "CLEAR" || !clearCovers(t, code, i, i+2, r) {
			t.Errorf("%s: discarded call result r%d not cleared right after the CALL:\n%s", fn, r, dis)
		}
	}

	_, body := funcDis(t, dis, "block")
	code := parseIns(t, body)
	ia, ra := callDst(t, code, 0, "g")
	ib, rb := callDst(t, code, ia+1, "g")
	pr := -1
	for i, in := range code {
		if in.op == "BUILTIN" && strings.HasSuffix(in.args, "PRINTLN") {
			pr = i
		}
	}
	for _, r := range []int{ra, rb} {
		if !clearCovers(t, code, ib, pr, r) {
			t.Errorf("block: local r%d not cleared at the end of its block:\n%s", r, dis)
		}
	}

	_, body = funcDis(t, dis, "loop")
	code = parseIns(t, body)
	ic, rc := callDst(t, code, 0, "g")
	back := -1
	for i := ic; i < len(code); i++ {
		if code[i].op == "JMP" && jumpTarget(t, code[i]) < code[i].pc {
			back = i
			break
		}
	}
	if back < 0 || !clearCovers(t, code, ic, back, rc) {
		t.Errorf("loop: body local r%d not cleared before the back edge:\n%s", rc, dis)
	}
}

// TestCompileClearKeepsLiveValues: clearing never kills a value that is
// still needed: loop-carried locals, locals of enclosing blocks, captured
// variables and two-result builtin targets survive.
func TestCompileClearKeepsLiveValues(t *testing.T) {
	dis := compileSrc(t, `package main

import "gogo/actor"

func g() any {
	var x any
	return x
}

func main() {
	x := g()
	for i := 0; i < 3; i++ {
		y := g()
		x = y
	}
	m, ok := actor.ReceiveTimeout(0)
	k := g()
	actor.Spawn(func() { println(k, m, ok) })
	println(x, m, ok, k)
}
`)
	_, body := funcDis(t, dis, "main")
	code := parseIns(t, body)
	// Locals: x=r0, i=r1 (loop), y=r2 (body), m=r1, ok=r2, k=r3. Before
	// the last println, x (r0) is never cleared, and m, ok, k (captured)
	// are not cleared after the two-result assignment.
	last := -1
	for i, in := range code {
		if in.op == "BUILTIN" && strings.HasSuffix(in.args, "PRINTLN") {
			last = i
		}
	}
	if clearCovers(t, code, -1, last, 0) {
		t.Errorf("x (r0) cleared while live:\n%s", dis)
	}
	recv := -1
	for i, in := range code {
		if in.op == "BUILTIN" && strings.HasSuffix(in.args, "RECEIVE_TIMEOUT") {
			recv = i
		}
	}
	for r := 1; r <= 2; r++ {
		if clearCovers(t, code, recv+2, last, r) {
			t.Errorf("r%d cleared after the two-result assignment while live:\n%s", r, dis)
		}
	}
	if ik, rk := callDst(t, code, recv, "g"); rk != 3 || clearCovers(t, code, ik, last, 3) {
		t.Errorf("captured k (r%d, want r3) cleared while live:\n%s", rk, dis)
	}
}

// TestCompileZeroPIDRef: zero PIDs and Refs are typed specials, not nil.
func TestCompileZeroPIDRef(t *testing.T) {
	dis := compileSrc(t, "package main\nimport \"gogo/actor\"\nfunc main() {\n\tvar p actor.PID\n\tvar r actor.Ref\n\tvar d actor.DownMsg\n\tprintln(p, r, d)\n}\n")
	for _, want := range []string{"LOADK r0, zeropid", "LOADK r1, zeroref"} {
		if !strings.Contains(dis, want) {
			t.Errorf("missing %q:\n%s", want, dis)
		}
	}
	if n := strings.Count(dis, "zeropid"); n != 2 { // p and DownMsg.From
		t.Errorf("%d zeropid loads, want 2:\n%s", n, dis)
	}
}

// TestCompileClearPeepholesKeepJumpTargets: moving and merging CLEARs
// around labels never leaves a jump target off an instruction boundary,
// and a CLEAR right before a label is not widened for the label's jumpers.
func TestCompileClearPeepholesKeepJumpTargets(t *testing.T) {
	dis := compileSrc(t, `package main

import "gogo/actor"

func g() any {
	var x any
	return x
}

func spin() {
	for g(); ; {
	}
}

func nested(a, b bool) {
	if a {
		if b {
			x := g()
			_ = x
		}
	}
}

func loops(n int) {
	for i := 0; i < n; i++ {
		x := g()
		if i == 1 {
			continue
		}
		for j := 0; j < n; j++ {
			y := g()
			if j == 2 {
				_ = y
				break
			}
		}
		_ = x
	}
}

func sw() {
	for {
		switch m := actor.Receive().(type) {
		case int:
			if m == 0 {
				break
			}
			_ = g()
		case string:
			return
		default:
			_ = g()
		}
	}
}

func main() {
	nested(true, false)
	loops(3)
	actor.Spawn(spin)
	actor.Spawn(sw)
}
`)
	for _, fn := range []string{"spin", "nested", "loops", "sw"} {
		_, body := funcDis(t, dis, fn)
		code := parseIns(t, body)
		starts := map[int]bool{}
		for _, in := range code {
			starts[in.pc] = true
		}
		for _, in := range code {
			if in.op == "JMP" || in.op == "JMPF" || in.op == "JMPT" {
				if tgt := jumpTarget(t, in); !starts[tgt] {
					t.Errorf("%s: jump at %d targets %d, not an instruction:\n%s", fn, in.pc, tgt, dis)
				}
			}
		}
	}
}

// TestClearPeepholesRespectLabels: a CLEAR with a label bound right after
// it is skipped by that label's jumpers, so it is neither dropped (before
// a return, or moved past a statement end) nor widened.
func TestClearPeepholesRespectLabels(t *testing.T) {
	newFn := func() *fn {
		return &fn{info: &funcInfo{name: "t"}, labelDirty: map[Label]int{}, boundAt: -1, lastClear: -1}
	}
	ops := func(f *fn) []uint8 {
		code, err := f.asm.Finish()
		if err != nil {
			t.Fatal(err)
		}
		var out []uint8
		for pc := 0; pc < len(code); pc++ {
			out = append(out, uint8(code[pc]))
			if OpHasExt(uint8(code[pc])) {
				pc++
			}
		}
		return out
	}
	// CLEAR; label; return: the CLEAR stays and the label stays on LOADK.
	f := newFn()
	f.alloc(nil)
	f.next = 0
	f.clearDead()
	l := f.asm.NewLabel()
	f.jump(OpJmp, 0, l) // a jumper, so the label is used
	f.bind(l)
	f.retNil()
	if got := ops(f); len(got) != 4 || got[0] != OpClear || got[2] != OpLoadk {
		t.Errorf("CLEAR; JMP; label; LOADK; RET expected, got ops %v", got)
	}
	// CLEAR; label (bind); bindEnd: not moved past the first label.
	f = newFn()
	f.alloc(nil)
	f.next = 0
	f.clearDead()
	l1, l2 := f.asm.NewLabel(), f.asm.NewLabel()
	f.bind(l1)
	f.bindEnd(l2)
	f.clearDead()
	f.retNil()
	if got := ops(f); len(got) != 3 || got[0] != OpClear {
		t.Errorf("CLEAR; LOADK; RET expected, got ops %v", got)
	}
	// Without a label in between, CLEARs merge and one before a plain
	// return is dropped.
	f = newFn()
	f.alloc(nil)
	f.alloc(nil)
	f.next = 1
	f.clearDead()
	f.next = 0
	f.clearDead()
	code := append([]uint32(nil), f.asm.code...)
	if len(code) != 1 || code[0] != makeWord(OpClear, 0, 2, 0) {
		t.Errorf("CLEAR r1,1 + CLEAR r0,1 not merged into CLEAR r0,2: %#x", code)
	}
	f.retNil()
	if got := ops(f); len(got) != 2 || got[0] != OpLoadk {
		t.Errorf("CLEAR before a bare return not dropped: %v", got)
	}
}

// TestCompileMultiResults: RETN returns consecutive registers; the call
// site gets one fresh destination register per result (cleared once dead
// like any call result); f(g()) passes them straight on as arguments; an
// int64 result going into any is boxed.
func TestCompileMultiResults(t *testing.T) {
	dis := compileSrc(t, `package main

func divmod(a, b int) (int, int) { return a / b, a % b }

func digits(n int) (tens, ones int) {
	tens = n / 10
	ones = n % 10
	return
}

func one() (n int) {
	n = 3
	return
}

func sum(a, b int) int { return a + b }

func raw() (int64, int64) { return 1, 2 }

func boxed() (any, int64) { return raw() }

func main() {
	q, r := divmod(7, 2)
	println(sum(divmod(9, 4)))
	divmod(1, 1)
	println(q, r, one())
	t, _ := digits(42)
	println(t)
}
`)
	_, body := funcDis(t, dis, "divmod")
	if code := parseIns(t, body); countOp(code, "RETN", "r2, 2") != 1 {
		t.Errorf("divmod: want RETN r2, 2:\n%s", dis)
	}
	_, body = funcDis(t, dis, "digits")
	if code := parseIns(t, body); countOp(code, "RETN", "r1, 2") != 1 {
		t.Errorf("digits: bare return of named results r1, r2 wants RETN r1, 2:\n%s", dis)
	}
	_, body = funcDis(t, dis, "one")
	if code := parseIns(t, body); countOp(code, "RET", "r0") != 1 || countOp(code, "RETN", "") != 0 {
		t.Errorf("one: a single named result returns with RET r0:\n%s", dis)
	}
	_, body = funcDis(t, dis, "boxed")
	code := parseIns(t, body)
	if len(code) < 3 || code[0].op != "CALL" || !strings.HasPrefix(code[0].args, "r0,") ||
		code[1].op != "BOX64" || code[1].args != "r0, r0" || code[2].op != "RETN" || code[2].args != "r0, 2" {
		t.Errorf("boxed: want CALL r0 ..raw; BOX64 r0, r0; RETN r0, 2:\n%s", dis)
	}

	_, body = funcDis(t, dis, "main")
	code = parseIns(t, body)
	// q=r0, r=r1; the results land in r2, r3 and move to q and r.
	i, dst := callDst(t, code, 0, "divmod")
	if dst != 2 || code[i+1].op != "MOVE" || code[i+1].args != "r0, r2" ||
		code[i+2].op != "MOVE" || code[i+2].args != "r1, r3" {
		t.Errorf("q, r := divmod(): want CALL r2 then MOVE r0, r2; MOVE r1, r3:\n%s", dis)
	}
	for _, r := range []int{2, 3} {
		if !clearCovers(t, code, i, i+5, r) {
			t.Errorf("result register r%d not cleared after the assignment:\n%s", r, dis)
		}
	}
	// sum(divmod(9, 4)): divmod's results are sum's arguments.
	j, d2 := callDst(t, code, i+1, "divmod")
	k, _ := callDst(t, code, j+1, "sum")
	if want := fmt.Sprintf("r%d, 2, sum", d2); !strings.HasSuffix(code[k].args, want) {
		t.Errorf("sum(divmod()): CALL sum args %q, want ...%s:\n%s", code[k].args, want, dis)
	}
	// divmod(1, 1) as a statement: two destination registers, cleared.
	l, d3 := callDst(t, code, k+1, "divmod")
	if !clearCovers(t, code, l, l+2, d3) || !clearCovers(t, code, l, l+2, d3+1) {
		t.Errorf("discarded results r%d, r%d not cleared:\n%s", d3, d3+1, dis)
	}
	last := -1
	for i, in := range code {
		if in.op == "BUILTIN" && strings.HasSuffix(in.args, "PRINTLN") {
			last = i
		}
	}
	for _, r := range []int{0, 1} {
		if clearCovers(t, code, -1, last, r) {
			t.Errorf("local r%d cleared while live:\n%s", r, dis)
		}
	}
}

// TestCompileByte: arithmetic producing a byte is truncated with CONV c=2,
// as is byte(x); int(b) and int64(b) are plain copies; comparisons are not
// truncated.
func TestCompileByte(t *testing.T) {
	dis := compileSrc(t, `package main

func f(c byte, x int, y int64) byte {
	d := c + 1
	c += 2
	c++
	e := byte(x)
	g := int(c)
	h := int64(c)
	k := byte(y)
	println(d < e, g, h, k)
	return -d
}

func main() { println(f(1, 2, 3)) }
`)
	_, body := funcDis(t, dis, "f")
	code := parseIns(t, body)
	n := 0
	for _, in := range code {
		if in.op == "CONV" {
			if !strings.HasSuffix(in.args, ", 2") {
				t.Errorf("unexpected %s %s:\n%s", in.op, in.args, dis)
			}
			n++
		}
	}
	// c + 1, c += 2, c++, byte(x), byte(y), -d
	if n != 6 {
		t.Errorf("%d CONV ..., 2, want 6:\n%s", n, dis)
	}
}

// TestCompileSlices: element writes, multi-value append (one APPEND, so
// growth is decided once), the struct zero built only on the
// growth path, nil as the nil slice, and slice temporaries cleared.
func TestCompileSlices(t *testing.T) {
	dis := compileSrc(t, `package main

type P struct{ X int }

func g() []int { return nil }

func set(s []int, i int) {
	s[i] = g()[0]
	println("x")
}

func many(s []int) []int {
	s = append(s, 1, 2, 3)
	return s
}

func structs(ps []P) []P {
	ps = append(ps, P{1})
	return ps
}

func isNil(s []int) bool { return s == nil }

func main() {
	set(nil, 0)
	many(nil)
	structs(nil)
	println(isNil(nil))
}
`)
	_, body := funcDis(t, dis, "set")
	code := parseIns(t, body)
	ig, rg := callDst(t, code, 0, "g")
	if countOp(code, "BUILTIN", "SETINDEX") != 1 {
		t.Errorf("set: want one SETINDEX:\n%s", dis)
	}
	if !clearCovers(t, code, ig, -1, rg) {
		t.Errorf("set: temporary r%d holding g()'s slice not cleared:\n%s", rg, dis)
	}

	_, body = funcDis(t, dis, "many")
	code = parseIns(t, body)
	if countOp(code, "BUILTIN", ", 6, APPEND") != 1 || countOp(code, "BUILTIN", "APPEND") != 1 {
		t.Errorf("many: want one APPEND of the slice, mode, zero and 3 values:\n%s", dis)
	}

	_, body = funcDis(t, dis, "structs")
	code = parseIns(t, body)
	jmpt, ns := -1, -1
	for i, in := range code {
		if in.op == "JMPT" && jmpt < 0 {
			jmpt = i
		}
		if in.op == "NEWSTRUCT" && ns < 0 {
			ns = i // the zero; the appended value's literal follows
		}
	}
	if countOp(code, "BUILTIN", "CAP") != 1 || jmpt < 0 || ns < jmpt || jumpTarget(t, code[jmpt]) <= code[ns].pc {
		t.Errorf("structs: zero struct not built only past the JMPT on len+n <= cap:\n%s", dis)
	}

	_, body = funcDis(t, dis, "isNil")
	if code = parseIns(t, body); countOp(code, "LOADK", "nilslice") != 1 || countOp(code, "EQ", "") != 1 {
		t.Errorf("isNil: want LOADK nilslice and EQ:\n%s", dis)
	}
}

// TestCompileBytes: gogo/bytes calls and methods become BUILTINs with the
// receiver first; the zero Bytes is emptybytes, the zero Buffer nil; case
// bytes.Bytes is a TYPEIS TyBytes.
func TestCompileBytes(t *testing.T) {
	dis := compileSrc(t, `package main

import "gogo/bytes"

// The zero Buffer the checker cannot see (a named result) is nil.
func zero() (nb bytes.Buffer) { return }

func main() {
	b := bytes.NewBuffer(4)
	b.WriteString("x")
	x := b.Freeze()
	var z bytes.Bytes
	println(x.Slice(0, 1).Len(), z.Equal(x), x.At(0), zero().Len())
	var a any = x
	switch a.(type) {
	case bytes.Bytes:
		println(1)
	}
}
`)
	for _, want := range []string{
		"BUILTIN r0, r1, 1, BUF_NEW",
		"BUILTIN r1, r2, 2, BUF_WRITE", "LOADK r0, nil",
		"BUF_FREEZE", "SLICE", "BYTES_EQUAL", "INDEX",
		", emptybytes", ", 3, SLICE",
		"TYPEIS", "TyBytes",
	} {
		if !strings.Contains(dis, want) {
			t.Errorf("missing %q in:\n%s", want, dis)
		}
	}
}

// A call of a func value is CALLV: the closure in a fresh register, the
// arguments right above it, the results in the destination; a literal's
// captures are its leading parameters, its own parameters follow.
func TestCompileCallValue(t *testing.T) {
	dis := compileSrc(t, `package main

func divmod(a, b int) (int, int) { return a / b, a % b }

func main() {
	k := 3
	add := func(x, y int) int { return x + y + k }
	println(add(1, 2))
	d := divmod
	q, r := d(7, 2)
	println(q, r)
}
`)
	if header, _ := funcDis(t, dis, "main.main.func1"); !strings.Contains(header, "params=3") {
		t.Errorf("func1 header %q, want params=3 (1 capture + 2 args)", header)
	}
	_, body := funcDis(t, dis, "main")
	text := strings.Join(body, "\n") + "\n"
	calls := regexp.MustCompile(`CALLV r(\d+), r(\d+), (\d+)\n`).FindAllStringSubmatch(text, -1)
	if len(calls) != 2 {
		t.Fatalf("%d CALLV in main, want 2:\n%s", len(calls), text)
	}
	for _, c := range calls {
		if c[3] != "2" {
			t.Errorf("CALLV %v: want 2 args", c)
		}
	}
}

// TestCompileBitopsTyped: the int vs int64 twin follows the left operand
// (for shifts, never the count), unary ^ is XOR with -1, and a byte result
// is truncated after every new op, compound assignment and s[i] op= e
// included.
func TestCompileBitopsTyped(t *testing.T) {
	dis := compileSrc(t, `package main

func ints(a, b int) int {
	return a&b | a ^ b&^a
}

func shifts(a int, x int64, n int, m int64, k byte) int64 {
	r := a << m  // SHL (left is int, count int64)
	r2 := a >> k // SHR
	x <<= k      // SHL64 (left int64, count byte)
	x = x >> n   // SHR64
	println(r, r2)
	return x
}

func untyped(n int) int { return 1 << n }

func main() {}

func neg(a int, x int64, b byte) {
	println(^a, ^x, ^b)
}

func bytes(b, m byte, s []byte) {
	println(b<<1, ^b, b&^m)
	b >>= 9
	b <<= 1
	s[0] |= m
	s[1] <<= 4
	println(b)
}
`)
	has := func(fn string, want map[string]int) {
		t.Helper()
		_, body := funcDis(t, dis, fn)
		code := parseIns(t, body)
		for op, n := range want {
			if got := countOp(code, op, ""); got != n {
				t.Errorf("%s: %d %s, want %d:\n%s", fn, got, op, n, dis)
			}
		}
	}
	has("ints", map[string]int{"AND": 1, "OR": 1, "XOR": 1, "ANDNOT": 1, "AND64": 0})
	has("shifts", map[string]int{"SHL": 1, "SHR": 1, "SHL64": 1, "SHR64": 1})
	has("untyped", map[string]int{"SHL": 1, "SHL64": 0})
	has("neg", map[string]int{"XOR": 2, "XOR64": 1, "CONV": 1})
	// Only b<<1, ^b, b<<=1, s[1]<<=4 end in CONV; b&^m, b>>=9, s[0]|=m cannot exceed 255.
	has("bytes", map[string]int{"SHL": 3, "SHR": 1, "XOR": 1, "ANDNOT": 1, "OR": 1, "CONV": 4})
}

// TestCompileFs checks the gogo/fs mapping: B_FS with the op code in the
// first register (functions and methods), File.Close as PORT_CLOSE, the
// File <-> PID conversion as a plain copy, and the reserved fs types.
func TestCompileFs(t *testing.T) {
	src := `package main

import (
	"gogo/actor"
	"gogo/fs"
	"gogo/port"
)

func main() {
	f, e := fs.Open("/flash/a", fs.Write)
	e = f.Write("x")
	n, e := f.Size()
	b, e := f.Read(4)
	e = f.Sync()
	f.Close()
	port.Close(actor.PID(f))
	info, e := fs.Stat("/flash")
	names, e := fs.ReadDir("/flash")
	e = fs.Rename("/flash/a", "/flash/b")
	println(n, b.Len(), info.Size, len(names), int(e))
}
`
	img := compileImage(t, src)
	dis, err := Disassemble(img)
	if err != nil {
		t.Fatal(err)
	}
	for _, re := range []string{
		`LOADK r\d+, 0\n\s+\d+\s+LOADK r\d+, "/flash/a"\n\s+\d+\s+LOADK r\d+, 1\n\s+\d+\s+BUILTIN r\d+, r\d+, 3, FS\n`, // Open(path, Write)
		`LOADK r\d+, 4\n\s+\d+\s+MOVE r\d+, r0\n\s+\d+\s+BUILTIN r\d+, r\d+, 2, FS\n`,                                  // f.Size()
		`LOADK r\d+, 11\n(?:.*\n){2}\s+\d+\s+BUILTIN r\d+, r\d+, 3, FS\n`,                                              // Rename
		`BUILTIN r\d+, r\d+, 1, PORT_CLOSE\n`,
	} {
		if !regexp.MustCompile(re).MatchString(dis) {
			t.Errorf("no match for %s:\n%s", re, dis)
		}
	}
	if n := strings.Count(dis, ", PORT_CLOSE\n"); n != 2 {
		t.Errorf("%d PORT_CLOSE, want 2 (f.Close and port.Close):\n%s", n, dis)
	}
	if n := strings.Count(dis, ", FS\n"); n != 8 {
		t.Errorf("%d FS builtins, want 8:\n%s", n, dis)
	}
}

func TestCompileKbdPowerTypes(t *testing.T) {
	img := compileImage(t, `package main

import (
	"gogo/actor"
	"gogo/kbd"
	"gogo/port"
	"gogo/power"
)

func main() {
	k, ok := port.Open("kbd", kbd.Config{RepeatDelayMs: 400})
	println(k, ok, kbd.ModCtrl, kbd.Fn, kbd.Enter)
	p, ok2 := port.Open("power", power.Config{PeriodMs: 1000})
	println(p, ok2)
	actor.Send(p, power.Read{})
	switch m := actor.Receive().(type) {
	case kbd.Key:
		println(m.Code, m.Char, m.Mods, m.Down)
	case power.Battery:
		println(m.MilliVolts, m.Pct, m.Charging, m.External)
	}
}
`)
	if v := binary.LittleEndian.Uint32(img[4:]); v != 15<<16 {
		t.Errorf("image version %s, want 15.0", VersionString(v))
	}
	seen := map[uint32]bool{}
	code := words(img)
	for i := 0; i+1 < len(code); i++ {
		if code[i]&0xFF == uint32(OpTypeis) {
			seen[code[i+1]] = true
		}
	}
	if !seen[TypeKbdKey] || !seen[TypePowerBattery] || TypeKbdKey != 26 || TypePowerBattery != 28 {
		t.Errorf("TYPEIS operands %v, want 26 and 28", seen)
	}
}

func TestCompileCode(t *testing.T) {
	img := compileImage(t, `package main

import (
	"gogo/actor"
	"gogo/bytes"
	"gogo/code"
)

func main() {
	m, e := code.Load(bytes.FromString("x"))
	p, e2 := code.Spawn(m, "Main", 1)
	p, e2 = code.SpawnLink(m, "Main", "s")
	p, e2 = code.SpawnOpt(m, "Main", 0, actor.Options{MaxMemory: 4096})
	println(p, e, e2, code.Hold(m), code.Missing(m), code.MemUsed(), m.H, code.BadVersion)
	code.Release(m)
	var a any = m
	switch x := a.(type) {
	case code.Module:
		println(x.H)
	}
}
`)
	dis, err := Disassemble(img)
	if err != nil {
		t.Fatal(err)
	}
	for _, re := range []string{
		`LOADK r\d+, 0\n(?:.*\n){1,2}\s+\d+\s+BUILTIN r\d+, r\d+, 2, CODE\n`, // Load(b)
		`LOADK r\d+, 3\n(?:.*\n){4,6}\s+\d+\s+BUILTIN r\d+, r\d+, 5, CODE\n`, // SpawnOpt(m, entry, arg, opts)
		`LOADK r\d+, 7\n\s+\d+\s+BUILTIN r\d+, r\d+, 1, CODE\n`,              // MemUsed()
	} {
		if !regexp.MustCompile(re).MatchString(dis) {
			t.Errorf("no match for %s:\n%s", re, dis)
		}
	}
	if n := strings.Count(dis, ", CODE\n"); n != 8 {
		t.Errorf("%d CODE builtins, want 8:\n%s", n, dis)
	}
	code := words(img)
	if !strings.Contains(dis, "TYPEIS") {
		t.Errorf("no TYPEIS:\n%s", dis)
	}
	found := false
	for i := 0; i+1 < len(code); i++ {
		if code[i]&0xFF == uint32(OpTypeis) && code[i+1] == TypeCodeModule {
			found = true
		}
	}
	if !found {
		t.Error("no TYPEIS code.Module (34)")
	}
}

// gogo/sys: the wall clock builtins; net.DialTLS is the tls port's config.
func TestCompileSysAndDialTLS(t *testing.T) {
	src := `package main

import (
	"gogo/net"
	"gogo/port"
	"gogo/sys"
)

func main() {
	set := sys.SetTime(1700000000000)
	now := sys.Time()
	println(now > 0, set, sys.Random())
	_, ok := port.Open("tls", net.DialTLS{IP: 1, Port: 443, TimeoutMs: 100, Host: "example.com"})
	println(ok)
}
`
	img := compileImage(t, src)
	dis, err := Disassemble(img)
	if err != nil {
		t.Fatal(err)
	}
	for _, re := range []string{
		`BUILTIN r\d+, r\d+, 1, SET_UNIX_TIME\n`,
		`BUILTIN r\d+, r\d+, 0, UNIX_TIME\n`,
		`BUILTIN r\d+, r\d+, 0, RANDOM\n`,
		`NEWSTRUCT r\d+, r\d+, 4, net\.DialTLS\n`,
	} {
		if !regexp.MustCompile(re).MatchString(dis) {
			t.Errorf("no match for %s:\n%s", re, dis)
		}
	}
}

// TestCompileControlFlow: an expression switch is an EQ (EQ64 for int64)
// and JMPT per case value, a local tag is compared in place, and the
// bodies follow in source order (fallthrough emits no jump); a variadic
// call with no variadic argument passes the nil slice.
func TestCompileControlFlow(t *testing.T) {
	dis := compileSrc(t, `package main

func n(xs ...int) int { return len(xs) }

func main() {
	x := 2
	switch x {
	case 1, 2:
		println("a")
		fallthrough
	case 3:
		println("b")
	}
	var w int64 = 3
	switch w {
	case 3:
		println("c")
	}
	println(n())
}
`)
	_, body := funcDis(t, dis, "main")
	code := parseIns(t, body)
	if countOp(code, "EQ", "r1, r0, ") != 3 || countOp(code, "JMPT", "") != 4 {
		t.Errorf("want 3 EQ on the tag in place and 4 JMPT:\n%s", dis)
	}
	if countOp(code, "EQ64", "") != 1 {
		t.Errorf("want EQ64 for the int64 switch:\n%s", dis)
	}
	if countOp(code, "MOVE", "r0") != 0 {
		t.Errorf("the local tag should not be copied:\n%s", dis)
	}
	if countOp(code, "JMP", "") != 2 { // after the tests of each switch; none for the fallthrough or the last body
		t.Errorf("want 2 JMP:\n%s", dis)
	}
	if countOp(code, "LOADK", "nilslice") != 1 {
		t.Errorf("want n() to pass the nil slice:\n%s", dis)
	}
}

// TestCompileMethods: a method is a function named Type.Method whose
// parameter 0 is the receiver; x.M(args) passes x right below the
// arguments, x.M is a closure capturing x, and T.M is the function itself.
func TestCompileMethods(t *testing.T) {
	dis := compileSrc(t, `package main

type P struct{ X, Y int }

type C int

func (p P) Add(q P) P { return P{p.X + q.X, p.Y + q.Y} }

func (p P) Sum(xs ...int) int { return p.X + len(xs) }

func (c C) Twice() C { return c * 2 }

func (C) Name() string {
	f := func() string { return "C" }
	return f()
}

func main() {
	p := P{1, 2}
	println(p.Add(p).X, p.Sum(), p.Sum(1, 2))
	f := p.Add
	g := P.Add
	println(f(p).Y, g(p, p).X, C(3).Twice(), C(1).Name())
}
`)
	for name, params := range map[string]string{"P.Add": "params=2", "P.Sum": "params=2", "C.Twice": "params=1", "C.Name": "params=1", "main.C.Name.func1": "params=0"} {
		if header, _ := funcDis(t, dis, name); !strings.Contains(header, params) {
			t.Errorf("%s header %q, want %s", name, header, params)
		}
	}
	_, body := funcDis(t, dis, "main")
	code := parseIns(t, body)
	// p.Add(p): the receiver (a copy of p) and then p, both above the locals.
	if countOp(code, "CALL", ", 2, P.Add") != 1 {
		t.Errorf("want one CALL of P.Add with 2 arguments:\n%s", dis)
	}
	// p.Sum() passes the receiver and the nil slice; p.Sum(1, 2) a new slice.
	if countOp(code, "CALL", ", 2, P.Sum") != 2 || countOp(code, "LOADK", "nilslice") != 1 {
		t.Errorf("want two CALLs of P.Sum with 2 arguments, one with the nil slice:\n%s", dis)
	}
	if countOp(code, "MKCLOSURE", ", 1, P.Add") != 1 || countOp(code, "MKCLOSURE", ", 0, P.Add") != 1 {
		t.Errorf("want p.Add capturing p and P.Add capturing nothing:\n%s", dis)
	}
}

// TestCompileLibraryMethods: a library's methods are named
// pkg.Type.Method, called and taken as values like main's.
func TestCompileLibraryMethods(t *testing.T) {
	libs := fakeLibs(t, map[string]string{"lib/lib.gg": `package lib

type Temp int

func (t Temp) F() int { return int(t)*9/5 + 32 }

type Box struct{ N int }

func (b Box) Get() int {
	g := func() int { return b.N }
	return g()
}
`})
	src := "package main\nimport \"gogo/lib\"\ntype Box struct{ N int }\nfunc (b Box) Get() int { return b.N }\nfunc main() {\n\tf := lib.Box{1}.Get\n\tprintln(lib.Temp(3).F(), f(), Box{2}.Get(), lib.Temp.F(1))\n}\n"
	prog, err := LoadWith(writeProg(t, src), libs)
	if err != nil {
		t.Fatalf("Load: %v", err)
	}
	img, err := Compile(prog)
	if err != nil {
		t.Fatalf("Compile: %v", err)
	}
	dis, err := Disassemble(img)
	if err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"lib.Temp.F", "lib.Box.Get", "lib.Box.Get.func1", "Box.Get"} {
		funcDis(t, dis, name)
	}
	for _, re := range []string{`CALL r\d+, r\d+, 1, lib\.Temp\.F\n`, `MKCLOSURE r\d+, r\d+, 1, lib\.Box\.Get\n`, `CALL r\d+, r\d+, 1, Box\.Get\n`} {
		if n := len(regexp.MustCompile(re).FindAllString(dis, -1)); n == 0 {
			t.Errorf("no %s:\n%s", re, dis)
		}
	}
}
