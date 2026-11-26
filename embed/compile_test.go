package embed

import (
	"bytes"
	"os"
	"strings"
	"sync"
	"testing"
	"testing/fstest"

	"gogotools/compiler"
)

const hello = "package main\n\nfunc main() {\n\tprintln(\"hello\")\n}\n"

// Compile's image is gogo build's for the same sources.
func TestCompileSameAsBuild(t *testing.T) {
	img, err := CompileDir("../examples/many_idle", nil)
	if err != nil {
		t.Fatal(err)
	}
	prog, err := compiler.Config{}.Load("../examples/many_idle")
	if err != nil {
		t.Fatal(err)
	}
	want, err := compiler.Compile(prog)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(img, want) {
		t.Errorf("CompileDir's image differs from compiler.Compile's")
	}
	src, err := os.ReadFile("../examples/many_idle/main.gg")
	if err != nil {
		t.Fatal(err)
	}
	img2, err := Compile(map[string][]byte{"main.gg": src}, nil)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(img2, want) {
		t.Errorf("Compile's image differs from compiler.Compile's")
	}
	img3, err := CompileFS(os.DirFS("../examples/many_idle"), nil)
	if err != nil || !bytes.Equal(img3, want) {
		t.Errorf("CompileFS's image differs from compiler.Compile's (%v)", err)
	}
	// One file, as gogo build x.gg.
	if _, err := CompileDir("../examples/many_idle/main.gg", nil); err != nil {
		t.Error(err)
	}
}

// User packages are subdirectories, imported under the root's name.
func TestCompileUserPackage(t *testing.T) {
	fsys := fstest.MapFS{
		"main.gg":   {Data: []byte("package main\n\nimport \"app/util\"\n\nfunc main() {\n\tprintln(util.Twice(21))\n}\n")},
		"util/u.gg": {Data: []byte("package util\n\nfunc Twice(x int) int { return 2 * x }\n")},
		"README":    {Data: []byte("not source")},
	}
	if _, err := CompileFS(fsys, &CompileOptions{Root: "app"}); err != nil {
		t.Fatal(err)
	}
	_, err := CompileFS(fsys, nil) // root "main": app/util is not found
	if err == nil || !strings.Contains(err.Error(), "app/util") {
		t.Errorf("without the root name: %v", err)
	}
}

// Errors name the files by their keys, not the temporary directory.
func TestCompileErrors(t *testing.T) {
	_, err := Compile(map[string][]byte{"main.gg": []byte("package main\n\nfunc main() {\n\tx := 1\n\tundefined_name()\n}\n")}, nil)
	if err == nil {
		t.Fatal("no error")
	}
	if msg := err.Error(); !strings.HasPrefix(msg, "main.gg:5:2: ") || strings.Contains(msg, os.TempDir()) {
		t.Errorf("message %q", msg)
	}
	for _, name := range []string{"../x.gg", "/abs.gg", "a\\b.gg", ""} {
		if _, err := Compile(map[string][]byte{name: []byte(hello)}, nil); err == nil {
			t.Errorf("file name %q accepted", name)
		}
	}
	if _, err := Compile(map[string][]byte{"main.gg": []byte(hello)}, &CompileOptions{Root: "a/b"}); err == nil {
		t.Errorf("root a/b accepted")
	}
	if _, err := Compile(map[string][]byte{"util/u.gg": []byte(hello)}, nil); err == nil || err.Error() != "main: no .gg files" {
		t.Errorf("no files: %v", err)
	}
}

func TestCompileWarnings(t *testing.T) {
	var warnings []string
	src := "package main\n\nimport \"gogo/port\"\n\nfunc open(name string) { _, _ = port.Open(name, port.Host{}) }\n\nfunc main() { open(\"x\") }\n"
	if _, err := Compile(map[string][]byte{"main.gg": []byte(src)}, &CompileOptions{Warn: func(w string) { warnings = append(warnings, w) }}); err != nil {
		t.Fatal(err)
	}
	if len(warnings) != 1 || !strings.HasPrefix(warnings[0], "main.gg:5:") {
		t.Errorf("warnings %q", warnings)
	}
}

// Compile may be called from many goroutines at once.
func TestCompileConcurrent(t *testing.T) {
	want, err := Compile(map[string][]byte{"main.gg": []byte(hello)}, nil)
	if err != nil {
		t.Fatal(err)
	}
	var wg sync.WaitGroup
	for i := 0; i < 8; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			img, err := Compile(map[string][]byte{"main.gg": []byte(hello)}, nil)
			if err != nil || !bytes.Equal(img, want) {
				t.Errorf("concurrent compile: %v", err)
			}
		}()
	}
	wg.Wait()
}
