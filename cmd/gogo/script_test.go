package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// An executable .gg file starting with #!/usr/bin/env -S gogo run runs as
// a script, with gogo found on PATH; one without the .gg suffix too.
func TestShebangScript(t *testing.T) {
	if err := exec.Command("/usr/bin/env", "-S", "true").Run(); err != nil {
		t.Skip("/usr/bin/env -S is not available")
	}
	vm := realVM(t)
	work := t.TempDir()
	bin := filepath.Join(work, "bin")
	if err := os.Mkdir(bin, 0o755); err != nil {
		t.Fatal(err)
	}
	buildGogo(t, bin)
	writeFiles(t, work, map[string]string{
		"scripts/calc/calc.gg": "package calc\n\nfunc Add(a, b int) int { return a + b }\n",
	})
	const src = "#!/usr/bin/env -S gogo run\n// A script.\npackage main\n\nimport \"scripts/calc\"\n\nfunc main() { println(\"script\", calc.Add(1, 2)) }\n"
	for _, name := range []string{"hello.gg", "hello"} {
		script := filepath.Join(work, "scripts", name)
		if err := os.WriteFile(script, []byte(src), 0o755); err != nil {
			t.Fatal(err)
		}
		cmd := exec.Command(script)
		cmd.Dir = work
		cmd.Env = append(os.Environ(), "PATH="+bin+string(os.PathListSeparator)+os.Getenv("PATH"), "GOGO_VM="+vm, "GOGO_CONFIG="+filepath.Join(work, "no-config"))
		out, err := cmd.CombinedOutput()
		if err != nil || string(out) != "script 3\n" {
			t.Errorf("%s: %v, output %q", name, err, out)
		}
	}
	// An error in a script names its real line.
	bad := filepath.Join(work, "scripts", "bad.gg")
	if err := os.WriteFile(bad, []byte("#!/usr/bin/env -S gogo run\npackage main\n\nfunc main() { nope() }\n"), 0o755); err != nil {
		t.Fatal(err)
	}
	cmd := exec.Command(bad)
	cmd.Env = append(os.Environ(), "PATH="+bin+string(os.PathListSeparator)+os.Getenv("PATH"), "GOGO_VM="+vm)
	out, err := cmd.CombinedOutput()
	if err == nil || !strings.Contains(string(out), "bad.gg:4:15: undefined: nope") {
		t.Errorf("bad.gg: %v, output %q", err, out)
	}
}
