package compiler

import (
	"sort"
	"strings"
	"testing"
	"testing/fstest"
)

// imgTrailer decodes the trailer of an image: the requirement names and
// the export names (name -> function index).
func imgTrailer(t *testing.T, raw []byte) ([]string, map[string]uint32) {
	t.Helper()
	w := words(raw)
	nf, nt, nc, ncode := w[2], w[3], w[4], w[5]
	consts := w[8+3*nf+2*nt:][:nc]
	str := func(off uint32) string {
		n := consts[off+1]
		var b []byte
		for k := uint32(0); k < n; k++ {
			b = append(b, byte(consts[off+2+k/4]>>(8*(k%4))))
		}
		return string(b)
	}
	tr := w[8+3*nf+2*nt+nc+ncode+(nc+31)/32:]
	if len(tr) < 1 || uint32(len(tr)) < 2+tr[0] {
		t.Fatalf("trailer is %d words", len(tr))
	}
	var req []string
	for _, off := range tr[1 : 1+tr[0]] {
		req = append(req, str(off))
	}
	tr = tr[1+tr[0]:]
	if uint32(len(tr)) != 1+2*tr[0] {
		t.Fatalf("exports are %d words, n_exports %d", len(tr), tr[0])
	}
	out := map[string]uint32{}
	for i := uint32(0); i < tr[0]; i++ {
		out[str(tr[1+2*i])] = tr[2+2*i]
	}
	return req, out
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
		want      string // the names, space-separated
		warnings  int
	}{
		// An import alone requires nothing: only the ports it opens.
		{"net import", "package main\nimport \"gogo/net\"\nfunc main() { _ = net.Up{} }\n", "", 0},
		{"kbd import", "package main\nimport \"gogo/kbd\"\nfunc main() { _ = kbd.Config{} }\n", "", 0},
		{"kbd driver", "package main\nimport (\n\"gogo/port\"\n\"gogo/kbd\"\n)\nfunc main() { _, _ = port.Open(\"kbd\", kbd.Config{}) }\n", "kbd", 0},
		{"kbd driver no import", "package main\nimport \"gogo/port\"\nfunc main() { _, _ = port.Open(\"kbd\", 1) }\n", "kbd", 0},
		{"power driver", "package main\nimport \"gogo/port\"\nfunc main() { _, _ = port.Open(\"power\", 1) }\n", "power", 0},
		{"tls driver", "package main\nimport \"gogo/port\"\nfunc main() { _, _ = port.Open(\"tls\", 1) }\n", "tls", 0},
		{"screen driver", "package main\nimport (\n\"gogo/port\"\n\"gogo/screen\"\n)\nfunc main() { _, _ = port.Open(\"screen\", screen.Config{}) }\n", "screen", 0},
		// Any name: a host port's, or one no board has (the load then fails).
		{"host port", "package main\nimport \"gogo/port\"\nfunc main() { _, _ = port.Open(\"ui\", port.Host{}) }\n", "ui", 0},
		{"two and a repeat", "package main\nimport \"gogo/port\"\nfunc main() {\n\t_, _ = port.Open(\"spk\", 1)\n\t_, _ = port.Open(\"gpio\", 1)\n\t_, _ = port.Open(\"spk\", 2)\n}\n", "gpio spk", 0},
		// gogo/http opens "tls" only for an https URL, and fails that
		// request cleanly without it: it declares //gogo:optional tls, so
		// importing it does not require tls; main's own open still does.
		{"http import", "package main\nimport \"gogo/http\"\nfunc main() { _, _ = http.Get(\"http://x/\", 0) }\n", "tcp tcp_listen udp", 0},
		{"http and own tls", "package main\nimport (\n\"gogo/http\"\n\"gogo/port\"\n)\nfunc main() {\n\t_, _ = http.Get(\"https://x/\", 0)\n\t_, _ = port.Open(\"tls\", 1)\n}\n", "tcp tcp_listen tls udp", 0},
		// gogo/sound declares //gogo:optional mic spk likewise.
		{"sound import", "package main\nimport \"gogo/sound\"\nfunc main() { _, _ = sound.Play(\"/flash/a.wav\") }\n", "", 0},
		{"sys import alone", "package main\nimport \"gogo/sys\"\nfunc main() { _ = sys.Time() }\n", "", 0},
		{"sd path", "package main\nimport \"gogo/fs\"\nfunc main() { _, _ = fs.ReadFile(\"/sd/a\", 10) }\n", "/sd", 0},
		{"sd exact", "package main\nimport \"gogo/fs\"\nfunc main() { _, _ = fs.ReadFile(\"/sd\", 10) }\n", "/sd", 0},
		{"sdx is not sd", "package main\nimport \"gogo/fs\"\nfunc main() { _, _ = fs.ReadFile(\"/sdx/a\", 10) }\n", "", 0},
		{"flash path", "package main\nimport \"gogo/fs\"\nfunc main() { _, _ = fs.ReadFile(\"/flash/a\", 10) }\n", "", 0},
		{"variable driver", "package main\nimport \"gogo/port\"\nfunc main() {\n\td := \"x\"\n\tif len(d) > 5 { d = \"y\" }\n\t_, _ = port.Open(d, 1)\n}\n", "", 1},
		{"plain gpio", "package main\nimport (\n\t\"gogo/gpio\"\n\t\"gogo/port\"\n)\nfunc main() { _, _ = port.Open(\"gpio\", gpio.Config{Pin: 1}) }\n", "gpio", 0},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			prog, err := Load(writeProg(t, tc.src))
			if err != nil {
				t.Fatalf("Load: %v", err)
			}
			names, warns := Requires(prog)
			if strings.Join(names, " ") != tc.want || len(warns) != tc.warnings {
				t.Errorf("Requires = %q, %v; want %q, %d warnings", names, warns, tc.want, tc.warnings)
			}
			for _, w := range warns {
				if !strings.HasSuffix(w, "main.gg:6:9: "+anyPortWarning) || !strings.Contains(w, "//gogo:optional") {
					t.Errorf("warning %q", w)
				}
			}
			img, err := Compile(prog)
			if err != nil {
				t.Fatalf("Compile: %v", err)
			}
			if got, _ := imgTrailer(t, img); strings.Join(got, " ") != tc.want {
				t.Errorf("image requires %q, want %q", got, tc.want)
			}
			if got, err := ImageRequires(img); err != nil || strings.Join(got, " ") != tc.want {
				t.Errorf("ImageRequires = %q, %v; want %q", got, err, tc.want)
			}
		})
	}
}

// TestOptionalDirective covers //gogo:optional: a library's own constant
// port.Open of a named driver (any name) adds no requirement; the same
// call in package main or in another library (an importer included) still
// does.
func TestOptionalDirective(t *testing.T) {
	const opt = "// Package a opens mic only when asked.\n//\n//gogo:optional mic spk ui\npackage a\nimport \"gogo/port\"\nfunc Rec() bool { _, ok := port.Open(\"mic\", 1); return ok }\nfunc Play() bool { _, ok := port.Open(\"spk\", 1); return ok }\nfunc UI() bool { _, ok := port.Open(\"ui\", port.Host{}); return ok }\nfunc IR() bool { _, ok := port.Open(\"ir_tx\", 1); return ok }\n"
	const user = "package b\nimport (\n\"gogo/a\"\n\"gogo/port\"\n)\nfunc Rec() bool { _, ok := port.Open(\"mic\", 1); return ok && a.Rec() }\n"
	libs := fakeLibs(t, map[string]string{"a/a.gg": opt, "b/b.gg": user})
	tests := []struct {
		name, src string
		want      string
	}{
		{"optional drivers", "package main\nimport \"gogo/a\"\nfunc main() { println(a.Rec(), a.Play(), a.UI()) }\n", ""},
		{"main opens it itself", "package main\nimport (\n\"gogo/a\"\n\"gogo/port\"\n)\nfunc main() {\n\tprintln(a.Rec())\n\t_, _ = port.Open(\"mic\", 1)\n}\n", "mic"},
		{"importer opens it itself", "package main\nimport \"gogo/b\"\nfunc main() { println(b.Rec()) }\n", "mic"},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			prog, err := LoadWith(writeProg(t, tc.src), libs)
			if err != nil {
				t.Fatalf("Load: %v", err)
			}
			// a.IR's ir_tx, not in the directive, always counts (Requires
			// reads every call of a compiled package, reachable or not).
			want := strings.TrimSpace("ir_tx " + tc.want)
			if names, _ := Requires(prog); strings.Join(names, " ") != want {
				t.Errorf("Requires = %q, want %q", names, want)
			}
		})
	}
}

// TestOptionalDirectiveMain: package main may mark its own constant
// port.Open drivers soft too; a library's open of the same name still
// counts, and the names it does not list count.
func TestOptionalDirectiveMain(t *testing.T) {
	const lib = "package a\nimport \"gogo/port\"\nfunc Ui() bool { _, ok := port.Open(\"ui\", port.Host{}); return ok }\n"
	libs := fakeLibs(t, map[string]string{"a/a.gg": lib})
	tests := []struct{ name, src, want string }{
		{"main soft", "// Program p.\n//\n//gogo:optional nosuch ui\npackage main\nimport \"gogo/port\"\nfunc main() {\n\t_, _ = port.Open(\"nosuch\", 1)\n\t_, _ = port.Open(\"ui\", port.Host{})\n\t_, _ = port.Open(\"gpio\", 1)\n}\n", "gpio"},
		{"library still counts", "//gogo:optional ui\npackage main\nimport (\n\"gogo/a\"\n\"gogo/port\"\n)\nfunc main() {\n\t_, _ = port.Open(\"ui\", port.Host{})\n\tprintln(a.Ui())\n}\n", "ui"},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			prog, err := LoadWith(writeProg(t, tc.src), libs)
			if err != nil {
				t.Fatalf("Load: %v", err)
			}
			names, warns := Requires(prog)
			if strings.Join(names, " ") != tc.want || len(warns) != 0 {
				t.Errorf("Requires = %q, %v; want %q", names, warns, tc.want)
			}
		})
	}
}

func TestOptionalDirectiveErrors(t *testing.T) {
	const useLib = "package main\nimport \"gogo/a\"\nfunc main() { a.F() }\n"
	lib := func(directive string) fstest.MapFS {
		return fakeLibs(t, map[string]string{"a/a.gg": "package a\n\n" + directive + "\n\nfunc F() {}\n"})
	}
	tests := []struct{ name, src, directive, want string }{
		{"no driver", useLib, "//gogo:optional", `a/a.gg:3:1: gogo: //gogo:optional names no driver`},
		{"unknown directive", useLib, "//gogo:optinal tls", `a/a.gg:3:1: gogo: unknown directive //gogo:optinal (want //gogo:optional)`},
		{"glued name", useLib, "//gogo:optionaltls", `unknown directive //gogo:optionaltls`},
		{"empty in main", "package main\n\n//gogo:optional\n\nfunc main() {}\n", "", `main.gg:3:1: gogo: //gogo:optional names no driver`},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			_, err := LoadWith(writeProg(t, tc.src), lib(tc.directive))
			if err == nil || !strings.Contains(err.Error(), tc.want) {
				t.Errorf("Load error %v, want %q", err, tc.want)
			}
		})
	}
}
