package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

// TestLibHost checks the gogo standard library (lib/) on the host Go
// toolchain. The library is gogo source (.gg), which the go tool does not
// read, so it is copied into a temporary Go module gogo: every
// lib/<pkg>/*.gg as <pkg>/*.go (import paths gogo/<pkg> then resolve as
// they do in the gogo compiler), and the host tests lib/<pkg>/*_test.go,
// which carry the build tag gogohost. Then go vet -stdmethods=false
// (gogo's bytes.Buffer.WriteByte deliberately returns no error) and go test
// run there with that tag.
func TestLibHost(t *testing.T) {
	goTool := filepath.Join(runtime.GOROOT(), "bin", "go")
	if _, err := os.Stat(goTool); err != nil {
		t.Skipf("no go tool at %s", goTool)
	}
	mod := t.TempDir()
	if err := os.WriteFile(filepath.Join(mod, "go.mod"), []byte("module gogo\n\ngo 1.27\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	files, err := filepath.Glob(filepath.Join("..", "lib", "*", "*"))
	if err != nil {
		t.Fatal(err)
	}
	n := 0
	for _, f := range files {
		name := filepath.Base(f)
		switch {
		case strings.HasSuffix(name, ".gg"):
			name = strings.TrimSuffix(name, ".gg") + ".go"
			n++
		case strings.HasSuffix(name, "_test.go"):
		default:
			continue
		}
		src, err := os.ReadFile(f)
		if err != nil {
			t.Fatal(err)
		}
		dir := filepath.Join(mod, filepath.Base(filepath.Dir(f)))
		if err := os.MkdirAll(dir, 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(dir, name), src, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	if n == 0 {
		t.Fatal("no lib/*/*.gg files")
	}
	for _, args := range [][]string{
		{"vet", "-tags", "gogohost", "-stdmethods=false", "./..."},
		{"test", "-count=1", "-tags", "gogohost", "./..."},
	} {
		cmd := exec.Command(goTool, args...)
		cmd.Dir = mod
		cmd.Env = append(os.Environ(), "GOWORK=off", "GOFLAGS=")
		if out, err := cmd.CombinedOutput(); err != nil {
			t.Errorf("go %s: %v\n%s", strings.Join(args, " "), err, out)
		} else {
			t.Logf("go %s:\n%s", strings.Join(args, " "), out)
		}
	}
}
