package compiler

import (
	"go/token"
	"go/types"
	"os"
	"regexp"
	"strconv"
	"strings"
	"testing"

	"gogotools/lib"
)

// TestFloatOpsMatchC cross-checks the B_FLOAT op codes with FLOAT_OP_* in
// runtime/floatlib/floatlib.h, and that every gogo/math function has one.
func TestFloatOpsMatchC(t *testing.T) {
	src, err := os.ReadFile("../runtime/floatlib/floatlib.h")
	if err != nil {
		t.Fatal(err)
	}
	c := map[string]int64{}
	for _, m := range regexp.MustCompile(`\b(FLOAT_OP_\w+)\s*=\s*(\d+)`).FindAllStringSubmatch(string(src), -1) {
		c[m[1]], _ = strconv.ParseInt(m[2], 10, 64)
	}
	if len(c) != len(floatOps) {
		t.Errorf("floatlib.h has %d FLOAT_OP_ codes, the compiler %d", len(c), len(floatOps))
	}
	for name, op := range floatOps {
		_, fn, _ := strings.Cut(name, ".")
		cname := "FLOAT_OP_" + strings.ToUpper(strings.TrimSuffix(strings.TrimSuffix(fn, "Float"), "ToEven"))
		switch fn {
		case "RoundToEven":
			cname = "FLOAT_OP_ROUNDEVEN"
		case "FormatFloat":
			cname = "FLOAT_OP_FORMAT"
		case "ParseFloat":
			cname = "FLOAT_OP_PARSE"
		}
		if v, ok := c[cname]; !ok || v != int64(op) {
			t.Errorf("%s: compiler %d, %s = %d (present %v)", name, op, cname, v, ok)
		}
	}
	l := &loader{fset: token.NewFileSet(), libs: lib.StubFS, pkgs: map[string]*types.Package{}}
	pkg, err := l.Import(MathPath)
	if err != nil {
		t.Fatal(err)
	}
	for _, name := range pkg.Scope().Names() {
		if fn, ok := pkg.Scope().Lookup(name).(*types.Func); ok {
			if _, ok := floatOps["math."+fn.Name()]; !ok {
				t.Errorf("math.%s has no B_FLOAT op", fn.Name())
			}
		}
	}
}

// TestFloatImage: a program with floats is stamped 15.2 and requires
// "float"; one that imports strconv without its float functions stays
// 15.0 and requires nothing (the intrinsics' bodies are not compiled).
func TestFloatImage(t *testing.T) {
	for _, tc := range []struct{ name, src, req string }{
		{"literal", "package main\nfunc main() {\n\tx := 1.5\n\tprintln(x)\n}\n", "float"},
		{"conversion", "package main\nfunc main() {\n\tn := 3\n\tprintln(float32(n))\n}\n", "float"},
		{"typeswitch", "package main\nfunc main() {\n\tvar a any = 1\n\tswitch a.(type) {\n\tcase float64:\n\t\tprintln(1)\n\t}\n}\n", "float"},
		{"math", "package main\nimport \"gogo/math\"\nfunc main() {\n\tprintln(math.IsNaN(math.NaN()))\n}\n", "float"},
		{"format", "package main\nimport \"gogo/strconv\"\nfunc main() {\n\t_, ok := strconv.ParseFloat(\"1\", 64)\n\tprintln(ok)\n}\n", "float"},
		{"strconv-ints", "package main\nimport \"gogo/strconv\"\nfunc main() {\n\tprintln(strconv.Itoa(3))\n}\n", ""},
		{"math-const", "package main\nimport \"gogo/math\"\nfunc main() {\n\tconst p = math.Pi * 100\n\tprintln(int(p - 0.159265358979323846264338327950288419716939937510582097494459))\n}\n", ""},
	} {
		img := compileImage(t, tc.src)
		minor := uint32(0)
		if tc.req != "" {
			minor = floatMinor
		}
		if v := words(img)[1]; v != 15<<16|minor {
			t.Errorf("%s: version %s, want 15.%d", tc.name, VersionString(v), minor)
		}
		if got, err := ImageRequires(img); err != nil || strings.Join(got, " ") != tc.req {
			t.Errorf("%s: requires %q (%v), want %q", tc.name, got, err, tc.req)
		}
	}
}

// TestCompileFloatOps: the float ops by static type, float32 and float64
// alike, and constants in the const area.
func TestCompileFloatOps(t *testing.T) {
	dis := compileSrc(t, "package main\nfunc f(a, b float32) bool { return a < b }\nfunc main() {\n\tx := 2.5\n\ty := x*x - 1\n\tvar z float32 = 0.5\n\tz /= 2\n\tprintln(-y, y > x, y != x, f(z, 1), int(y), float32(y), float64(z), int64(x))\n}\n")
	for _, want := range []string{"MULF", "SUBF", "DIVF", "NEGF", "LTF", "NEF", "CONVF", "float64(2.5)", "float32(0.5)"} {
		if !strings.Contains(dis, want) {
			t.Errorf("no %s in\n%s", want, dis)
		}
	}
	for _, bad := range []string{" ADD ", " MUL ", " LT ", " NE "} {
		if strings.Contains(dis, bad) {
			t.Errorf("int op %q on floats in\n%s", bad, dis)
		}
	}
}
