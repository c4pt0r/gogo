package compiler

import (
	"strings"
	"testing"
)

// TestSizedImage: a program stamps 15.3 (sizedMinor) exactly when it uses
// what the sized integers added: an unsigned 64-bit op, a CONV mode above
// byte's (uint16, int8, int16, widening a uint32) or B_PRINTLNU. byte
// arithmetic and int32 stay at 15.0.
func TestSizedImage(t *testing.T) {
	for _, tc := range []struct {
		name, src string
		sized     bool
	}{
		{"uint64-div", "package main\nfunc main() {\n\tvar a, b uint64 = 7, 2\n\tprintln(int64(a / b))\n}\n", true},
		{"uint32-lt", "package main\nfunc main() {\n\tvar a, b uint32 = 7, 2\n\tprintln(a < b)\n}\n", true},
		{"uint32-println", "package main\nfunc main() {\n\tvar a uint32 = 7\n\tprintln(a)\n}\n", true},
		{"int8-add", "package main\nfunc main() {\n\tvar a int8 = 7\n\ta += a\n\tprintln(int(a))\n}\n", true},
		{"uint32-add", "package main\nfunc main() {\n\tvar a uint32 = 7\n\ta += a\n\tprintln(int(a))\n}\n", false},
		{"byte", "package main\nfunc main() {\n\tvar a byte = 7\n\ta += a\n\tprintln(a, a/3, a > 2)\n}\n", false},
		{"int32", "package main\nfunc main() {\n\tvar a int32 = 7\n\ta *= a\n\tprintln(a, a/3, a > 2)\n}\n", false},
	} {
		img := compileImage(t, tc.src)
		want := uint32(15 << 16)
		if tc.sized {
			want |= sizedMinor
		}
		if v := words(img)[1]; v != want {
			t.Errorf("%s: version %s, want %s", tc.name, VersionString(v), VersionString(want))
		}
	}
}

// TestCompileSizedOps: the ops chosen by static type.
func TestCompileSizedOps(t *testing.T) {
	for _, tc := range []struct {
		name, src string
		want, bad []string
	}{
		{"uint64", "package main\nfunc f(a, b uint64) bool { return a < b && a <= b }\nfunc main() {\n\tvar a, b uint64 = 7, 2\n\tprintln(a/b, a%b, a>>b, a*b, f(a, b), a > b)\n}\n",
			[]string{"DIVU64", "REMU64", "SHRU64", "MUL64", "LTU64", "LEU64", "PRINTLNU"}, []string{" DIV64 ", " LT64 ", " SHR64 "}},
		{"uint32", "package main\nfunc main() {\n\tvar a, b uint32 = 7, 2\n\tprintln(a/b, a%b, a>>b, a*b, a < b)\n}\n",
			[]string{"CONV r", "DIV64", "REM64", "SHR64", " MUL ", "LT64", "PRINTLNU"}, []string{"DIVU64", " DIV ", " LT "}},
		{"int16", "package main\nfunc main() {\n\tvar a, b int16 = 7, 2\n\tprintln(a*b, a/b, a&b, a<<b)\n}\n",
			[]string{" MUL ", " DIV ", " SHL ", ", 5\n"}, []string{"PRINTLNU", "MUL64"}},
	} {
		dis := compileSrc(t, tc.src)
		for _, w := range tc.want {
			if !strings.Contains(dis, w) {
				t.Errorf("%s: no %q in\n%s", tc.name, w, dis)
			}
		}
		for _, b := range tc.bad {
			if strings.Contains(dis, b) {
				t.Errorf("%s: %q in\n%s", tc.name, b, dis)
			}
		}
	}
}

// TestPrintlnUint64Limit: B_PRINTLNU's mask has 30 bits.
func TestPrintlnUint64Limit(t *testing.T) {
	err := compileErr(t, "package main\nfunc main() {\n\tvar u uint64\n\tprintln(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, u)\n}\n")
	if !strings.Contains(err.Error(), "uint64 after the 30th argument") {
		t.Errorf("error %v", err)
	}
	compileSrc(t, "package main\nfunc main() {\n\tvar u uint32\n\tprintln(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, u)\n}\n")
}
