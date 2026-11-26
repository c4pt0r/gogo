package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"
)

// SIGTERM to `gogo run` is forwarded to the VM, which is waited for; the
// temporary image directory is removed afterwards.
func TestRunForwardsSIGTERM(t *testing.T) {
	work := t.TempDir()
	tmp := filepath.Join(work, "tmp")
	if err := os.Mkdir(tmp, 0o755); err != nil {
		t.Fatal(err)
	}
	prog := filepath.Join(work, "prog")
	if err := os.Mkdir(prog, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(prog, "main.gg"), []byte("package main\n\nfunc main() { println(\"x\") }\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	// A fake VM: records its pid, and on SIGTERM records it and exits 7.
	pidFile := filepath.Join(work, "pid")
	termFile := filepath.Join(work, "term")
	vm := filepath.Join(work, "fakevm.sh")
	script := "#!/bin/sh\necho $$ > " + pidFile + ".tmp && mv " + pidFile + ".tmp " + pidFile +
		"\ntrap 'echo got > " + termFile + "; exit 7' TERM\nwhile :; do sleep 0.05; done\n"
	if err := os.WriteFile(vm, []byte(script), 0o755); err != nil {
		t.Fatal(err)
	}
	gogo := filepath.Join(work, "gogo")
	if out, err := exec.Command("go", "build", "-o", gogo, ".").CombinedOutput(); err != nil {
		t.Fatalf("build: %v\n%s", err, out)
	}
	cmd := exec.Command(gogo, "run", prog)
	cmd.Env = append(os.Environ(), "GOGO_VM="+vm, "TMPDIR="+tmp)
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	var vmPid int
	for deadline := time.Now().Add(10 * time.Second); time.Now().Before(deadline); time.Sleep(20 * time.Millisecond) {
		if b, err := os.ReadFile(pidFile); err == nil {
			vmPid, _ = strconv.Atoi(strings.TrimSpace(string(b)))
			if vmPid > 0 {
				break
			}
		}
	}
	if vmPid == 0 {
		cmd.Process.Kill()
		t.Fatal("fake VM never started")
	}
	if ents, _ := os.ReadDir(tmp); len(ents) != 1 {
		t.Fatalf("temp dir entries while running: %v", ents)
	}
	cmd.Process.Signal(syscall.SIGTERM)
	done := make(chan error, 1)
	go func() { done <- cmd.Wait() }()
	select {
	case err := <-done:
		if ee, ok := err.(*exec.ExitError); !ok || ee.ExitCode() != 7 {
			t.Fatalf("gogo run exit = %v, want status 7 from the VM", err)
		}
	case <-time.After(10 * time.Second):
		cmd.Process.Kill()
		t.Fatal("gogo run did not exit after SIGTERM")
	}
	if _, err := os.Stat(termFile); err != nil {
		t.Error("the VM did not receive SIGTERM")
	}
	if err := syscall.Kill(vmPid, 0); err == nil {
		t.Error("the VM child is still alive")
	}
	if ents, _ := os.ReadDir(tmp); len(ents) != 0 {
		t.Errorf("temp dir not cleaned up: %v", ents)
	}
}

func TestRunKeyNeedsValue(t *testing.T) {
	if _, err := runCmd([]string{"--key"}); err == nil || !strings.Contains(err.Error(), "flag needs an argument: -key") {
		t.Errorf("err = %v", err)
	}
	if _, err := runCmd([]string{"--key", "a=1"}); err == nil || !strings.Contains(err.Error(), "missing program") {
		t.Errorf("err = %v", err)
	}
	if _, err := runCmd([]string{"--key", "a=1", "--key"}); err == nil || !strings.Contains(err.Error(), "needs an argument") {
		t.Errorf("err = %v", err)
	}
	if _, err := runCmd(nil); err == nil || !strings.Contains(err.Error(), "missing program") {
		t.Errorf("err = %v", err)
	}
}

func TestRunFsRoot(t *testing.T) {
	if _, err := runCmd([]string{"--fs-root"}); err == nil || !strings.Contains(err.Error(), "needs an argument: -fs-root") {
		t.Errorf("err = %v", err)
	}
	work := t.TempDir()
	args := filepath.Join(work, "args")
	vm := filepath.Join(work, "fakevm.sh")
	if err := os.WriteFile(vm, []byte("#!/bin/sh\nprintf '%s\\n' \"$@\" > "+args+"\n"), 0o755); err != nil {
		t.Fatal(err)
	}
	img := filepath.Join(work, "x.ggb")
	if err := os.WriteFile(img, []byte("img"), 0o644); err != nil {
		t.Fatal(err)
	}
	t.Setenv("GOGO_VM", vm)
	if code, err := runCmd([]string{"--key", "a=1", "--fs-root", "/some/dir", img}); err != nil || code != 0 {
		t.Fatalf("code %d err %v", code, err)
	}
	b, _ := os.ReadFile(args)
	want := "--key\na=1\n--fs-root\n/some/dir\n" + img + "\n"
	if string(b) != want {
		t.Errorf("vm args = %q, want %q", b, want)
	}
	// The same with single dashes and "=" values.
	if code, err := runCmd([]string{"-key", "a=1", "-fs-root=/some/dir", img}); err != nil || code != 0 {
		t.Fatalf("code %d err %v", code, err)
	}
	if b, _ := os.ReadFile(args); string(b) != want {
		t.Errorf("vm args = %q, want %q", b, want)
	}
}

// gogo run exits with the VM's status: main's exit, a refused image, a
// build error (needs the VM: make test-go sets GOGO_VM).
func TestRunExitCodes(t *testing.T) {
	vm := os.Getenv("GOGO_VM")
	if vm == "" {
		t.Skip("GOGO_VM is not set (make test-go sets it)")
	}
	dir := t.TempDir()
	prog := func(name, src string) string {
		p := filepath.Join(dir, name+".gg")
		if err := os.WriteFile(p, []byte("package main\n\n"+src), 0o644); err != nil {
			t.Fatal(err)
		}
		return p
	}
	junk := filepath.Join(dir, "junk.ggb")
	if err := os.WriteFile(junk, []byte("not an image"), 0o644); err != nil {
		t.Fatal(err)
	}
	// A VM without the keyboard, as on a board without one.
	noKbd := filepath.Join(dir, "vm-nokbd.sh")
	if err := os.WriteFile(noKbd, []byte("#!/bin/sh\nexec "+vm+" --without kbd \"$@\"\n"), 0o755); err != nil {
		t.Fatal(err)
	}
	const actorImport = "import \"gogo/actor\"\n\n"
	cases := []struct {
		vm, target string
		want       int
	}{
		{vm, prog("ok", "func main() {}\n"), exitOK},
		{vm, prog("user", actorImport+"func main() { actor.Exit(123) }\n"), 123},
		{vm, prog("lib", actorImport+"func main() { actor.Exit(actor.Failed + 5) }\n"), 69},
		{vm, prog("crash", "func main() {\n\tz := 0\n\tprintln(1 / z)\n}\n"), exitCrashed},
		{noKbd, prog("missing", "import \"gogo/port\"\n\nfunc main() { _, _ = port.Open(\"kbd\", 1) }\n"), exitMissing},
		{vm, prog("broken", "func main() { x := 1 }\n"), exitUsage},
		{vm, junk, exitBadImage},
	}
	for _, c := range cases {
		t.Setenv("GOGO_VM", c.vm)
		code, err := runCmd([]string{c.target})
		if code != c.want {
			t.Errorf("gogo run %s: exit %d (%v), want %d", filepath.Base(c.target), code, err, c.want)
		}
	}
}
