package tests

import (
	"bytes"
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"

	"gogotools/compiler"
)

const (
	footprintTarget = 160 // 32-bit bytes: warn above
	footprintLimit  = 256 // 32-bit bytes: fail above
	actor32Size     = 48  // sizeof(Actor) on 32-bit
)

// TestIdleActorFootprint spawns 1000 typical idle actors and checks the
// memory of one of them against the 32-bit targets, raised by the host's
// sizeof(Actor) - 48.
func TestIdleActorFootprint(t *testing.T) {
	vm := os.Getenv("GOGO_VM")
	if vm == "" {
		t.Fatal("GOGO_VM is not set: run `make test-go`, or build the VM with `make vm` and set GOGO_VM=$PWD/build/gogovm")
	}
	out, err := exec.Command(vm, "--sizeof-actor").Output()
	if err != nil {
		t.Fatalf("--sizeof-actor: %v", err)
	}
	sizeofActor, err := strconv.Atoi(strings.TrimSpace(string(out)))
	if err != nil {
		t.Fatalf("parse --sizeof-actor output %q: %v", out, err)
	}
	delta := sizeofActor - actor32Size

	prog, err := compiler.Load("footprint")
	if err != nil {
		t.Fatalf("load: %v", err)
	}
	img, err := compiler.Compile(prog)
	if err != nil {
		t.Fatalf("compile: %v", err)
	}
	imgPath := filepath.Join(t.TempDir(), "footprint.gogo")
	if err := os.WriteFile(imgPath, img, 0o644); err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	var stdout, stderr bytes.Buffer
	cmd := exec.CommandContext(ctx, vm, imgPath)
	cmd.Stdout = &stdout
	cmd.Stderr = &stderr
	cmd.WaitDelay = time.Second
	if err := cmd.Run(); err != nil {
		t.Fatalf("VM failed: %v\n--- stdout ---\n%s\n--- stderr ---\n%s", err, stdout.Bytes(), stderr.Bytes())
	}
	m, err := strconv.Atoi(strings.TrimSpace(stdout.String()))
	if err != nil {
		t.Fatalf("parse program output %q: %v", stdout.String(), err)
	}
	t.Logf("idle actor: %d bytes measured (sizeof(Actor)=%d, delta=%d); 32-bit equivalent: %d bytes (target %d, limit %d)",
		m, sizeofActor, delta, m-delta, footprintTarget, footprintLimit)
	if m > footprintLimit+delta {
		t.Errorf("idle actor %d bytes > hard limit %d (32-bit equivalent %d > %d)", m, footprintLimit+delta, m-delta, footprintLimit)
	}
	if m > footprintTarget+delta {
		t.Logf("WARNING: idle actor %d bytes > target %d", m, footprintTarget+delta)
	}
}
