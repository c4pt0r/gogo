// Package nettest runs network scenarios: each test
// compiles a gogo program from progs/, starts it on the VM named by
// $GOGO_VM with --net-fd 3 --sim-clock over a socketpair, and plays the
// peer with package sim. Simulated time makes timeouts and retransmits
// run instantly and deterministically.
package nettest

import (
	"io"
	"os"
	"path/filepath"
	"testing"
	"time"

	"gogotools/compiler"
	"gogotools/tests/net/sim"
)

// realLimit bounds each VM child in real time.
const realLimit = 10 * time.Second

// build compiles progs/<name> into an image in a temp dir.
func build(t *testing.T, name string) string {
	t.Helper()
	prog, err := compiler.Load(filepath.Join("progs", name))
	if err != nil {
		t.Fatalf("load %s: %v", name, err)
	}
	img, err := compiler.Compile(prog)
	if err != nil {
		t.Fatalf("compile %s: %v", name, err)
	}
	p := filepath.Join(t.TempDir(), name+".gogo")
	if err := os.WriteFile(p, img, 0o644); err != nil {
		t.Fatal(err)
	}
	return p
}

// start builds progs/<name> and starts it linked to a simulator, with
// extra VM arguments (--fs-root DIR); the VM is waited for (and its output
// logged on failure) at cleanup.
func start(t *testing.T, name string, extra ...string) *sim.VM {
	t.Helper()
	vmPath := os.Getenv("GOGO_VM")
	if vmPath == "" {
		t.Fatal("GOGO_VM is not set: run `make test-go`, or build the VM with `make vm` and set GOGO_VM=$PWD/build/gogovm")
	}
	v, err := sim.Start(vmPath, build(t, name), realLimit, extra...)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		v.Wait()
		if t.Failed() {
			t.Logf("--- stdout ---\n%s--- stderr ---\n%s", v.Stdout.Bytes(), v.Stderr.Bytes())
		}
	})
	return v
}

// finish expects the VM to have exited by itself with status 0 and
// stdout want.
func finish(t *testing.T, v *sim.VM, want string) {
	t.Helper()
	if !v.Closed() {
		t.Error("VM still running (link open)")
	}
	if err := v.Wait(); err != nil {
		t.Errorf("VM: %v (want exit status 0)", err)
	}
	if got := v.Stdout.String(); got != want {
		t.Errorf("stdout\n--- got ---\n%s--- want ---\n%s", got, want)
	}
}

// The VM boots on the link with an image that opens nothing and exits.
func TestHarnessBoot(t *testing.T) {
	v := start(t, "boot")
	if _, err := v.Sync(); err != io.EOF {
		t.Fatalf("sync: %v, want EOF (VM exited)", err)
	}
	finish(t, v, "booted\n")
}

// 10 s of simulated time pass instantly; timers fire at their own times
// within one large advance; the VM acknowledges an advance once idle; a
// frame sent to a VM that opened no net port is dropped harmlessly.
func TestHarnessSimClock(t *testing.T) {
	v := start(t, "ticks")
	t0 := time.Now()
	if _, err := v.Sync(); err != nil { // idle in its first Sleep at time 0
		t.Fatalf("sync: %v", err)
	}
	fr, err := v.Advance(3000) // tick at 2000, then idle until 4000
	if err != nil || len(fr) != 0 {
		t.Fatalf("advance 3000: %v %d frames", err, len(fr))
	}
	if err := v.Send(sim.Pad([]byte{1, 2, 3})); err != nil {
		t.Fatal(err)
	}
	if _, err := v.Sync(); err != nil {
		t.Fatalf("sync after frame: %v", err)
	}
	if _, err := v.Advance(7000); err != io.EOF { // ticks at 4000..10000, then exit
		t.Fatalf("advance 7000: %v, want EOF", err)
	}
	if v.Now() != 10000 {
		t.Fatalf("now %d", v.Now())
	}
	if d := time.Since(t0); d > 2*time.Second {
		t.Errorf("10 s of simulated time took %v of real time", d)
	}
	finish(t, v, "start 0\ntick 2000\ntick 4000\ntick 6000\ntick 8000\ntick 10000\n")
}

// Closing the link ends a VM that waits on the simulated clock.
func TestHarnessLinkClosed(t *testing.T) {
	v := start(t, "ticks")
	if _, err := v.Sync(); err != nil {
		t.Fatal(err)
	}
	if err := v.Wait(); err != nil {
		t.Fatalf("VM: %v (want exit 0 on link EOF)", err)
	}
	if got := v.Stdout.String(); got != "start 0\n" {
		t.Fatalf("stdout %q", got)
	}
}

// TestMain points the configuration store (config.Get) at a file that
// does not exist, never ~/.gogo/config; tests that need one set their own.
func TestMain(m *testing.M) {
	dir, err := os.MkdirTemp("", "gogo-nettest-")
	if err != nil {
		panic(err)
	}
	os.Setenv("GOGO_CONFIG", filepath.Join(dir, "no-config"))
	code := m.Run()
	os.RemoveAll(dir)
	os.Exit(code)
}
