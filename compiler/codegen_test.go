package compiler

import (
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
	// Out{} zero value: In{0}, "" and false fields.
	if !strings.Contains(dis, `LOADK r`) || !strings.Contains(dis, `""`) || !strings.Contains(dis, "false") {
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
