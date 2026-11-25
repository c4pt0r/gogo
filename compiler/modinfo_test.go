package compiler

import (
	"sort"
	"strings"
	"testing"
)

// imgTrailer decodes the v9 trailer of an image: requires and the export
// names (name -> function index).
func imgTrailer(t *testing.T, raw []byte) (uint32, map[string]uint32) {
	t.Helper()
	w := words(raw)
	nf, nt, nc, ncode := w[2], w[3], w[4], w[5]
	consts := w[8+3*nf+2*nt:][:nc]
	tr := w[8+3*nf+2*nt+nc+ncode+(nc+31)/32:]
	if len(tr) < 2 || uint32(len(tr)) != 2+2*tr[1] {
		t.Fatalf("trailer is %d words, n_exports %d", len(tr), tr[1])
	}
	out := map[string]uint32{}
	for i := uint32(0); i < tr[1]; i++ {
		off := tr[2+2*i]
		n := consts[off+1]
		var b []byte
		for k := uint32(0); k < n; k++ {
			b = append(b, byte(consts[off+2+k/4]>>(8*(k%4))))
		}
		out[string(b)] = tr[3+2*i]
	}
	return tr[0], out
}

func exportNames(m map[string]uint32) []string {
	var s []string
	for k := range m {
		s = append(s, k)
	}
	sort.Strings(s)
	return s
}

func TestExports(t *testing.T) {
	src := `package main
import "gogo/actor"
func main() {}
func Main(arg any) { _ = arg }
func Tick() {}
func helper() {}
func Bad(x int) { _ = x }
func Res() int { return 1 }
func Two(a, b any) { _, _ = a, b }
func init0() { actor.Spawn(Tick) }
`
	_, ex := imgTrailer(t, compileImage(t, src))
	if got := strings.Join(exportNames(ex), ","); got != "Main,Tick,main" {
		t.Errorf("exports %s, want Main,Tick,main", got)
	}
	// Each export names its function in the image.
	img := compileImage(t, src)
	w := words(img)
	_, ex = imgTrailer(t, img)
	if ex["main"] != w[6] {
		t.Errorf("main exported as func %d, entry %d", ex["main"], w[6])
	}

	// Without main the package still builds.
	_, ex = imgTrailer(t, compileImage(t, "package main\nfunc Main(arg any) { _ = arg }\nfunc Tick() {}\nfunc helper() {}\n"))
	if got := strings.Join(exportNames(ex), ","); got != "Main,Tick" {
		t.Errorf("exports without main %s, want Main,Tick", got)
	}
}

func TestRequiresInference(t *testing.T) {
	tests := []struct {
		name, src string
		bits      uint32
		warnings  int
	}{
		{"net import", "package main\nimport \"gogo/net\"\nfunc main() { _ = net.Up{} }\n", CapNet, 0},
		{"kbd driver", "package main\nimport (\n\"gogo/port\"\n\"gogo/kbd\"\n)\nfunc main() { _, _ = port.Open(\"kbd\", kbd.Config{}) }\n", CapKbd, 0},
		{"kbd driver no import", "package main\nimport \"gogo/port\"\nfunc main() { _, _ = port.Open(\"kbd\", 1) }\n", CapKbd, 0},
		{"power driver", "package main\nimport \"gogo/port\"\nfunc main() { _, _ = port.Open(\"power\", 1) }\n", CapPower, 0},
		{"udp driver", "package main\nimport \"gogo/port\"\nfunc main() { _, _ = port.Open(\"udp\", 1) }\n", CapNet, 0},
		{"sd path", "package main\nimport \"gogo/fs\"\nfunc main() { _, _ = fs.ReadFile(\"/sd/a\", 10) }\n", CapSD, 0},
		{"sd exact", "package main\nimport \"gogo/fs\"\nfunc main() { _, _ = fs.ReadFile(\"/sd\", 10) }\n", CapSD, 0},
		{"sdx is not sd", "package main\nimport \"gogo/fs\"\nfunc main() { _, _ = fs.ReadFile(\"/sdx/a\", 10) }\n", 0, 0},
		{"flash path", "package main\nimport \"gogo/fs\"\nfunc main() { _, _ = fs.ReadFile(\"/flash/a\", 10) }\n", 0, 0},
		{"variable driver", "package main\nimport \"gogo/port\"\nfunc main() {\n\td := \"x\"\n\tif len(d) > 5 { d = \"y\" }\n\t_, _ = port.Open(d, 1)\n}\n", CapAnyPort, 1},
		{"plain gpio", "package main\nimport \"gogo/port\"\nfunc main() { _, _ = port.Open(\"gpio\", port.GPIO{Pin: 1}) }\n", 0, 0},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			prog, err := Load(writeProg(t, tc.src))
			if err != nil {
				t.Fatalf("Load: %v", err)
			}
			bits, warns := Requires(prog)
			if bits != tc.bits || len(warns) != tc.warnings {
				t.Errorf("Requires = %#x, %v; want %#x, %d warnings", bits, warns, tc.bits, tc.warnings)
			}
			for _, w := range warns {
				if w != "port.Open with a non-constant driver name: requires any port" {
					t.Errorf("warning %q", w)
				}
			}
			img, err := Compile(prog)
			if err != nil {
				t.Fatalf("Compile: %v", err)
			}
			if got, _ := imgTrailer(t, img); got != tc.bits {
				t.Errorf("image requires %#x, want %#x", got, tc.bits)
			}
		})
	}
}

func TestCapConstants(t *testing.T) {
	if CapNet != 1 || CapKbd != 2 || CapPower != 4 || CapSD != 8 || CapAnyPort != 1<<31 {
		t.Error("capability bits changed")
	}
}
