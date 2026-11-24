package compiler

import (
	"encoding/binary"
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
	want := []string{"ProcInfo/1", "Reason/2", "ExitMsg/2", "DownMsg/3", "Request/3", "Options/1"}
	// Emitted even when the program does not import gogo/actor.
	img := compileImage(t, "package main\ntype P struct{ X, Y int }\nfunc main() {\n\tvar a any = P{1, 2}\n\tswitch a.(type) {\n\tcase P:\n\t\tprintln(1)\n\t}\n}\n")
	got := imageTypes(t, img)
	if strings.Join(got, " ") != strings.Join(append(want, "P/2"), " ") {
		t.Fatalf("types %v, want %v then P/2", got, want)
	}
	// TYPEIS P names index 6.
	code := words(img)
	found := false
	for i := 0; i+1 < len(code); i++ {
		if code[i]&0xFF == uint32(OpTypeis) && code[i+1] == 6 {
			found = true
		}
	}
	if !found {
		t.Error("no TYPEIS with type id 6")
	}
	// Using the actor types keeps them at their reserved indices.
	img = compileImage(t, "package main\nimport \"gogo/actor\"\ntype U struct{ A int }\nfunc main() {\n\tu := U{1}\n\tvar o actor.Options\n\tvar r actor.Reason\n\tprintln(u, o, r, actor.Info(actor.Self()).Memory)\n}\n")
	if got := imageTypes(t, img); strings.Join(got, " ") != strings.Join(append(want, "U/1"), " ") {
		t.Fatalf("types %v, want %v then U/1", got, want)
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

// A two-result builtin outside a two-value assignment (normally stopped
// by the checker) is an internal error in codegen, not a write to dst+1.
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
	g.nameLits([]*ast.FuncDecl{fd})
	err = g.compileFunc(&funcInfo{name: "main"}, nil, fd.Body)
	if err == nil || !strings.Contains(err.Error(), "internal error") || !strings.Contains(err.Error(), "ReceiveTimeout") {
		t.Fatalf("want an internal error naming ReceiveTimeout, got %v", err)
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
	want := map[int64]bool{1 << 40: false, -5: false, 0: false, 1: false}
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
	libs := fakeLibs(t, map[string]string{"lib/lib.go": `package lib

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
	want := "ProcInfo/1 Reason/2 ExitMsg/2 DownMsg/3 Request/3 Options/1"
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
