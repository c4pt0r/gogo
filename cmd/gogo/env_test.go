package main

import (
	"bytes"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

func TestEnv(t *testing.T) {
	work := t.TempDir()
	a, b := filepath.Join(work, "a"), filepath.Join(work, "b")
	sep := string(os.PathListSeparator)
	t.Setenv("GOGO_VM", "/opt/vm/gogovm")

	t.Setenv("GOGOPATH", a+sep+b)
	var out bytes.Buffer
	if err := envCmd(nil, &out); err != nil {
		t.Fatal(err)
	}
	want := []string{
		"version   " + version + " (image format ",
		"GOGOPATH  " + a + sep + b + " ($GOGOPATH)\n",
		"search    1. the build root",
		"          2. " + a + "\n",
		"          3. " + b + "\n",
		"gogovm    /opt/vm/gogovm ($GOGO_VM)\n",
	}
	for _, w := range want {
		if !strings.Contains(out.String(), w) {
			t.Errorf("gogo env lacks %q:\n%s", w, out.String())
		}
	}

	out.Reset()
	if err := envCmd([]string{"-path", b}, &out); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(out.String(), "GOGOPATH  "+b+" (-path)\n") || strings.Contains(out.String(), a) {
		t.Errorf("gogo env -path:\n%s", out.String())
	}

	os.Unsetenv("GOGOPATH")
	t.Setenv("HOME", work)
	out.Reset()
	if err := envCmd(nil, &out); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(out.String(), "GOGOPATH  "+filepath.Join(work, "gogo")+" ($HOME/gogo, GOGOPATH is unset)\n") {
		t.Errorf("gogo env, GOGOPATH unset:\n%s", out.String())
	}
	if err := envCmd([]string{"x"}, &out); err == nil {
		t.Error("gogo env x: no error")
	}
}

// lookVM: gogovm (or its release asset name) next to gogo, else the
// repository's build/gogovm, else an error that says what to do.
func TestLookVM(t *testing.T) {
	work := t.TempDir()
	touch := func(p string) {
		t.Helper()
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(p, nil, 0o755); err != nil {
			t.Fatal(err)
		}
	}
	bin := filepath.Join(work, "bin")
	repo := filepath.Join(work, "repo")
	touch(filepath.Join(repo, "go.mod"))
	os.WriteFile(filepath.Join(repo, "go.mod"), []byte("module gogotools\n"), 0o644)
	sub := filepath.Join(repo, "examples", "x")
	os.MkdirAll(sub, 0o755)
	os.MkdirAll(bin, 0o755)

	// Nothing next to gogo, outside the repository: an error.
	if _, _, err := lookVM(bin, work); err == nil || !strings.Contains(err.Error(), "cannot find the VM") || !strings.Contains(err.Error(), vmAsset) {
		t.Errorf("err = %v", err)
	}
	// Inside the repository without build/gogovm: says make vm.
	if _, _, err := lookVM(bin, sub); err == nil || !strings.Contains(err.Error(), "make vm") {
		t.Errorf("err = %v", err)
	}
	touch(filepath.Join(repo, "build", vmName))
	if vm, from, err := lookVM(bin, sub); err != nil || vm != filepath.Join(repo, "build", vmName) || from != "the gogo repository" {
		t.Errorf("repo: %q %q %v", vm, from, err)
	}
	// The release asset name next to gogo comes before the repository.
	touch(filepath.Join(bin, vmAsset))
	if vm, from, err := lookVM(bin, sub); err != nil || vm != filepath.Join(bin, vmAsset) || from != "next to gogo" {
		t.Errorf("asset: %q %q %v", vm, from, err)
	}
	// gogovm next to gogo comes first.
	touch(filepath.Join(bin, vmName))
	if vm, _, err := lookVM(bin, sub); err != nil || vm != filepath.Join(bin, vmName) {
		t.Errorf("next to gogo: %q %v", vm, err)
	}
	// $GOGO_VM comes before everything.
	t.Setenv("GOGO_VM", "/x/vm")
	if vm, from, err := findVM(); err != nil || vm != "/x/vm" || from != "$GOGO_VM" {
		t.Errorf("GOGO_VM: %q %q %v", vm, from, err)
	}
}

// buildGogo builds the gogo command into dir.
func buildGogo(t *testing.T, dir string) string {
	t.Helper()
	gogo := filepath.Join(dir, "gogo")
	if out, err := exec.Command("go", "build", "-o", gogo, ".").CombinedOutput(); err != nil {
		t.Fatalf("build: %v\n%s", err, out)
	}
	return gogo
}

// realVM returns the host VM ($GOGO_VM, else the repository's
// build/gogovm), or skips the test.
func realVM(t *testing.T) string {
	t.Helper()
	vm := os.Getenv("GOGO_VM")
	if vm == "" {
		vm, _ = filepath.Abs(filepath.Join("..", "..", "build", "gogovm"))
	}
	if _, err := os.Stat(vm); err != nil {
		t.Skip("no host VM (make vm)")
	}
	return vm
}

func writeFiles(t *testing.T, dir string, files map[string]string) {
	t.Helper()
	for name, src := range files {
		p := filepath.Join(dir, filepath.FromSlash(name))
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(p, []byte(src), 0o644); err != nil {
			t.Fatal(err)
		}
	}
}

// gogo run of a directory runs all its .gg files; of a .gg file, that file
// alone, its directory being the build root for user packages; -path is
// the search path.
func TestRunSources(t *testing.T) {
	vm := realVM(t)
	work := t.TempDir()
	gogo := buildGogo(t, work)
	writeFiles(t, work, map[string]string{
		"prog/main.gg":          "package main\n\nimport \"prog/util\"\n\nfunc main() { println(\"dir\", util.Two(), helper()) }\n",
		"prog/helper.gg":        "package main\n\nfunc helper() int { return 3 }\n",
		"prog/util/u.gg":        "package util\n\nfunc Two() int { return 2 }\n",
		"prog/hello.txt":        "not read",
		"s/hello.gg":            "package main\n\nfunc main() { println(\"hello\") }\n",
		"s/other.gg":            "package main\n\nfunc main() { println(\"other\") }\n",
		"s/args.gg":             "package main\n\nimport \"gogo/sys\"\n\nfunc main() {\n\targs := sys.Args()\n\tprintln(len(args))\n\tfor _, a := range args {\n\t\tprintln(\"[\" + a + \"]\")\n\t}\n}\n",
		"s/sum.gg":              "package main\n\nimport \"s/calc\"\n\nfunc main() { println(\"sum\", calc.Add(2, 3)) }\n",
		"s/calc/calc.gg":        "package calc\n\nfunc Add(a, b int) int { return a + b }\n",
		"s/gp.gg":               "package main\n\nimport \"shared/greet\"\n\nfunc main() { println(greet.Hi()) }\n",
		"gp/shared/greet/g.gg":  "package greet\n\nfunc Hi() string { return \"hi from GOGOPATH\" }\n",
		"gp2/shared/greet/g.gg": "package greet\n\nfunc Hi() string { return \"hi from -path\" }\n",
	})
	run := func(env []string, args ...string) (string, string, error) {
		cmd := exec.Command(gogo, append([]string{"run"}, args...)...)
		cmd.Env = append(os.Environ(), append([]string{"GOGO_VM=" + vm, "GOGO_CONFIG=" + filepath.Join(work, "no-config")}, env...)...)
		var out, errb bytes.Buffer
		cmd.Stdout, cmd.Stderr = &out, &errb
		err := cmd.Run()
		return out.String(), errb.String(), err
	}
	gp := "GOGOPATH=" + filepath.Join(work, "gp")
	tests := []struct {
		args []string
		want string
	}{
		{[]string{filepath.Join(work, "prog")}, "dir 2 3\n"},
		{[]string{filepath.Join(work, "s", "hello.gg")}, "hello\n"},
		{[]string{filepath.Join(work, "s", "sum.gg")}, "sum 5\n"},
		{[]string{filepath.Join(work, "s", "gp.gg")}, "hi from GOGOPATH\n"},
		{[]string{"-path", filepath.Join(work, "gp2"), filepath.Join(work, "s", "gp.gg")}, "hi from -path\n"},
	}
	for _, tc := range tests {
		out, errs, err := run([]string{gp}, tc.args...)
		if err != nil || out != tc.want || errs != "" {
			t.Errorf("gogo run %q: %v, stdout %q (want %q), stderr %q", tc.args, err, out, tc.want, errs)
		}
	}
	// The directory s has two func mains: running it fails.
	if _, errs, err := run([]string{gp}, filepath.Join(work, "s")); err == nil || !strings.Contains(errs, "main redeclared") {
		t.Errorf("gogo run s: %v %q", err, errs)
	}
	// Everything after the program is the program's: sys.Args, flags
	// look-alikes included, and none of it changes how the program is found.
	for _, extra := range [][]string{nil, {"x"}, {"-v", "a b", "", "--", "-path"}} {
		out, errs, err := run(nil, append([]string{filepath.Join(work, "s", "args.gg")}, extra...)...)
		want := fmt.Sprintf("%d\n", len(extra))
		for _, a := range extra {
			want += "[" + a + "]\n"
		}
		if err != nil || out != want || errs != "" {
			t.Errorf("gogo run args.gg %q: %v, stdout %q (want %q), stderr %q", extra, err, out, want, errs)
		}
	}
	// gogo build of a .gg file writes <name>.ggb.
	cmd := exec.Command(gogo, "build", filepath.Join(work, "s", "hello.gg"))
	cmd.Dir = work
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("build hello.gg: %v %s", err, out)
	}
	if _, err := os.Stat(filepath.Join(work, "hello.ggb")); err != nil {
		t.Error(err)
	}
}

// A package in the build root and on the search path: gogo build warns,
// naming both.
func TestBuildWarnsShadowed(t *testing.T) {
	work := t.TempDir()
	writeFiles(t, work, map[string]string{
		"app/main.gg":      "package main\n\nimport \"app/util\"\n\nfunc main() { util.F() }\n",
		"app/util/u.gg":    "package util\n\nfunc F() {}\n",
		"gp/app/util/u.gg": "package util\n\nfunc F() {}\n",
	})
	gogo := buildGogo(t, work)
	cmd := exec.Command(gogo, "build", "-path", filepath.Join(work, "gp"), "app", "-o", "app.ggb")
	cmd.Dir = work
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("%v %s", err, out)
	}
	want := `gogo: warning: package "app/util" is in 2 places; using the first: ` + filepath.Join("app", "util") + ", " + filepath.Join(work, "gp", "app", "util")
	if !strings.Contains(string(out), want) {
		t.Errorf("output %q\nlacks %q", out, want)
	}
}
