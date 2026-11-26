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
// way (integers, bools, strings) and import no gogo package. Skipped when
// no go command is found.
func TestGoldenMatchesGo(t *testing.T) {
	gobin, err := exec.LookPath("go")
	if err != nil {
		t.Skip("no go command in PATH")
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
			cmd.Env = append(os.Environ(), "GOTOOLCHAIN=local", "GOFLAGS=", "GOWORK=off")
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
