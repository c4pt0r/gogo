// Package tests holds end-to-end tests that compile gogo programs and run
// them on the C VM.
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

	"gogo/compiler"
)

// TestGolden compiles every tests/golden/*/ program, runs it on the VM named
// by $GOGO_VM (30s timeout) and requires exit status 0 and stdout equal
// byte-for-byte to expected.out; if expected.err exists, each of its
// non-empty lines must be a substring of stderr, and no non-empty line of
// forbidden.err may be. An `input` file is fed to stdin. `make test-go`
// builds the VM and sets GOGO_VM.
func TestGolden(t *testing.T) {
	vm := os.Getenv("GOGO_VM")
	if vm == "" {
		t.Fatal("GOGO_VM is not set: run `make test-go`, or build the VM with `make vm` and set GOGO_VM=$PWD/build/gogovm")
	}
	dirs, err := filepath.Glob(filepath.Join("golden", "*"))
	if err != nil {
		t.Fatal(err)
	}
	if len(dirs) == 0 {
		t.Fatal("no golden programs found")
	}
	for _, dir := range dirs {
		if fi, err := os.Stat(dir); err != nil || !fi.IsDir() {
			continue
		}
		t.Run(filepath.Base(dir), func(t *testing.T) {
			want, err := os.ReadFile(filepath.Join(dir, "expected.out"))
			if err != nil {
				t.Fatal(err)
			}
			prog, err := compiler.Load(dir)
			if err != nil {
				t.Fatalf("load: %v", err)
			}
			img, err := compiler.Compile(prog)
			if err != nil {
				t.Fatalf("compile: %v", err)
			}
			imgPath := filepath.Join(t.TempDir(), filepath.Base(dir)+".gogo")
			if err := os.WriteFile(imgPath, img, 0o644); err != nil {
				t.Fatal(err)
			}
			ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
			defer cancel()
			var stdout, stderr bytes.Buffer
			cmd := exec.CommandContext(ctx, vm, imgPath)
			cmd.Stdout = &stdout
			cmd.Stderr = &stderr
			// An optional `input` file is stdin (simulated gpio events);
			// otherwise stdin is /dev/null.
			if in, err := os.Open(filepath.Join(dir, "input")); err == nil {
				defer in.Close()
				cmd.Stdin = in
			} else if !os.IsNotExist(err) {
				t.Fatal(err)
			}
			cmd.WaitDelay = time.Second // don't wait on pipes held by orphans
			runErr := cmd.Run()
			if ctx.Err() != nil {
				t.Fatalf("VM timed out after 30s\n--- stdout ---\n%s\n--- stderr ---\n%s", stdout.Bytes(), stderr.Bytes())
			}
			if runErr != nil {
				t.Errorf("VM failed: %v (want exit status 0)\n--- stderr ---\n%s", runErr, stderr.Bytes())
			}
			if !bytes.Equal(stdout.Bytes(), want) {
				t.Errorf("stdout mismatch\n--- got ---\n%s\n--- want ---\n%s\n--- stderr ---\n%s",
					stdout.Bytes(), want, stderr.Bytes())
			}
			// Optional expected.err: each non-empty line must appear
			// somewhere in stderr.
			wantErr, err := os.ReadFile(filepath.Join(dir, "expected.err"))
			if err != nil && !os.IsNotExist(err) {
				t.Fatal(err)
			}
			for _, line := range strings.Split(string(wantErr), "\n") {
				if line != "" && !strings.Contains(stderr.String(), line) {
					t.Errorf("stderr lacks %q\n--- stderr ---\n%s", line, stderr.Bytes())
				}
			}
			// Optional forbidden.err: no non-empty line may appear in stderr.
			noErr, err := os.ReadFile(filepath.Join(dir, "forbidden.err"))
			if err != nil && !os.IsNotExist(err) {
				t.Fatal(err)
			}
			for _, line := range strings.Split(string(noErr), "\n") {
				if line != "" && strings.Contains(stderr.String(), line) {
					t.Errorf("stderr has forbidden %q\n--- stderr ---\n%s", line, stderr.Bytes())
				}
			}
		})
	}
}
