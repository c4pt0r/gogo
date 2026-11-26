package tests

import (
	"bytes"
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// TestGoldenMatchesGo runs every tests/golden/*/ program that has a file
// `gocompare` as a Go program, with the go command: its .gg files are
// copied as .go files into a temporary module, and what Go's println
// prints (to stderr) must equal the golden's expected.out, which the gogo
// run is held to by TestGolden. So such a golden shows that gogo means
// what Go means: it may print only values Go's println prints the same
// way (integers, bools, strings) and import no gogo package; a marked
// golden must be one package that exits 0 with no input, arguments or VM
// setup (no expected.err, progargs, exitcode, ... and no subdirectories).
// Skipped when no go command (Go 1.22 or later) is found. The program is
// built for the host whatever GOOS, GOARCH or GOEXPERIMENT say.
func TestGoldenMatchesGo(t *testing.T) {
	gobin, err := exec.LookPath("go")
	if err != nil {
		t.Skip("no go command in PATH")
	}
	env := append(os.Environ(), "GOTOOLCHAIN=local", "GOFLAGS=", "GOWORK=off", "GOOS=", "GOARCH=", "GOEXPERIMENT=")
	ver := exec.Command(gobin, "env", "GOVERSION")
	ver.Env = env
	out, err := ver.Output()
	if err != nil {
		t.Skipf("go env GOVERSION: %v", err)
	}
	if !goAtLeast(strings.TrimSpace(string(out)), 22) {
		t.Skipf("%s is older than go1.22", strings.TrimSpace(string(out)))
	}
	marks, err := filepath.Glob(filepath.Join("golden", "*", "gocompare"))
	if err != nil {
		t.Fatal(err)
	}
	if len(marks) == 0 {
		t.Fatal("no golden program has a gocompare file")
	}
	for _, mark := range marks {
		dir := filepath.Dir(mark)
		t.Run(filepath.Base(dir), func(t *testing.T) {
			t.Parallel()
			ents, err := os.ReadDir(dir)
			if err != nil {
				t.Fatal(err)
			}
			for _, e := range ents {
				switch {
				case e.IsDir():
					t.Fatalf("gocompare: %s has a subdirectory %s; only a single-package program can run as Go", dir, e.Name())
				case unsupportedMarks[e.Name()]:
					t.Fatalf("gocompare: %s has %s; a program compared with Go runs with no input, arguments or VM setup and exits 0", dir, e.Name())
				}
			}
			want, err := os.ReadFile(filepath.Join(dir, "expected.out"))
			if err != nil {
				t.Fatal(err)
			}
			srcs, err := filepath.Glob(filepath.Join(dir, "*.gg"))
			if err != nil || len(srcs) == 0 {
				t.Fatalf("no .gg files in %s (%v)", dir, err)
			}
			mod := t.TempDir()
			for _, src := range srcs {
				b, err := os.ReadFile(src)
				if err != nil {
					t.Fatal(err)
				}
				name := strings.TrimSuffix(filepath.Base(src), ".gg") + ".go"
				if err := os.WriteFile(filepath.Join(mod, name), b, 0o644); err != nil {
					t.Fatal(err)
				}
			}
			if err := os.WriteFile(filepath.Join(mod, "go.mod"), []byte("module gocompare\n\ngo 1.22\n"), 0o644); err != nil {
				t.Fatal(err)
			}
			ctx, cancel := context.WithTimeout(context.Background(), 2*time.Minute)
			defer cancel()
			var stdout, stderr bytes.Buffer
			cmd := exec.CommandContext(ctx, gobin, "run", ".")
			cmd.Dir = mod
			cmd.Stdout = &stdout
			cmd.Stderr = &stderr
			cmd.Env = env
			if err := cmd.Run(); err != nil {
				t.Fatalf("go run: %v\n--- stderr ---\n%s", err, stderr.Bytes())
			}
			if stdout.Len() > 0 {
				t.Errorf("go run printed to stdout (only println output is compared):\n%s", stdout.Bytes())
			}
			if !bytes.Equal(stderr.Bytes(), want) {
				t.Errorf("Go prints\n%s\nexpected.out (gogo) is\n%s", stderr.Bytes(), want)
			}
		})
	}
}

// unsupportedMarks are the golden files TestGolden acts on that a Go run
// cannot reproduce.
var unsupportedMarks = map[string]bool{
	"expected.err": true, "expected.tail": true, "forbidden.err": true, "progargs": true,
	"exitcode": true, "exitfail": true, "vmargs": true, "input": true,
	"fsroot": true, "config": true,
}

// goAtLeast reports whether version ("go1.24.1", "go1.22rc1") is Go 1.minor
// or later.
func goAtLeast(version string, minor int) bool {
	rest, ok := strings.CutPrefix(version, "go1.")
	if !ok {
		return false
	}
	n := 0
	for _, c := range rest {
		if c < '0' || c > '9' {
			break
		}
		n = n*10 + int(c-'0')
	}
	return n >= minor
}
