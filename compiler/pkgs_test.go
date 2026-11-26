package compiler

import (
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
)

// writeTree writes files (slash paths relative to dir) under dir and
// returns dir.
func writeTree(t *testing.T, dir string, files map[string]string) string {
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
	return dir
}

// buildRun loads, checks and compiles dir with search path path.
func buildRun(t *testing.T, dir string, path []string) (*Program, error) {
	t.Helper()
	prog, err := Config{Path: path}.Load(dir)
	if err != nil {
		return nil, err
	}
	if errs := Check(prog); len(errs) > 0 {
		return nil, errs[0]
	}
	if _, err := Compile(prog); err != nil {
		return nil, err
	}
	return prog, nil
}

const utilPkg = "package util\n\ntype Point struct{ X, Y int }\n\nfunc Sum(p Point) int { return p.X + p.Y }\n"

func TestUserPackages(t *testing.T) {
	work := t.TempDir()
	app := writeTree(t, filepath.Join(work, "app"), map[string]string{
		"main.gg":     "package main\n\nimport (\n\t\"app/a/b\"\n\t\"app/util\"\n)\n\nfunc main() { println(util.Sum(util.Point{X: 1, Y: 2}), b.F()) }\n",
		"util/u.gg":   utilPkg,
		"a/b/b.gg":    "package b\n\nimport \"app/util\"\n\nfunc F() int { return util.Sum(util.Point{X: 3}) }\n",
		"a/b/note.go": "this is not read",
	})
	prog, err := buildRun(t, app, nil)
	if err != nil {
		t.Fatal(err)
	}
	var paths []string
	for _, l := range prog.Libs {
		paths = append(paths, l.Pkg.Path())
	}
	if got := strings.Join(paths, " "); got != "app/util app/a/b" {
		t.Errorf("packages %q, want dependency order \"app/util app/a/b\"", got)
	}
	if len(prog.Warnings) != 0 {
		t.Errorf("warnings %v", prog.Warnings)
	}
	// The root given as "." works the same (its base name is the prefix).
	wd, _ := os.Getwd()
	defer os.Chdir(wd)
	if err := os.Chdir(app); err != nil {
		t.Fatal(err)
	}
	if _, err := buildRun(t, ".", nil); err != nil {
		t.Errorf("build of .: %v", err)
	}
}

func TestSearchPathPackages(t *testing.T) {
	work := t.TempDir()
	use := "package main\n\nimport \"lib/util\"\n\nfunc main() { println(util.Sum(util.Point{})) }\n"
	app := writeTree(t, filepath.Join(work, "app"), map[string]string{"main.gg": use})
	p1 := writeTree(t, filepath.Join(work, "p1"), map[string]string{"lib/util/u.gg": utilPkg})
	p2 := writeTree(t, filepath.Join(work, "p2"), map[string]string{"lib/util/u.gg": utilPkg + "\nfunc Two() {}\n"})
	empty := filepath.Join(work, "empty")

	prog, err := buildRun(t, app, []string{empty, p1})
	if err != nil {
		t.Fatal(err)
	}
	if len(prog.Warnings) != 0 {
		t.Errorf("warnings %v", prog.Warnings)
	}
	// Two directories hold it: the first wins, with a warning naming both.
	prog, err = buildRun(t, app, []string{p2, p1})
	if err != nil {
		t.Fatal(err)
	}
	if prog.Libs[0].Pkg.Scope().Lookup("Two") == nil {
		t.Error("the second search path directory won")
	}
	if len(prog.Warnings) != 1 || !strings.Contains(prog.Warnings[0], filepath.Join(p2, "lib", "util")+", "+filepath.Join(p1, "lib", "util")) {
		t.Errorf("warnings %v", prog.Warnings)
	}
}

// The build root comes before the search path, with a warning.
func TestRootShadowsSearchPath(t *testing.T) {
	work := t.TempDir()
	app := writeTree(t, filepath.Join(work, "app"), map[string]string{
		"main.gg":   "package main\n\nimport \"app/util\"\n\nfunc main() { util.Root() }\n",
		"util/u.gg": "package util\n\nfunc Root() {}\n",
	})
	gp := writeTree(t, filepath.Join(work, "gp"), map[string]string{"app/util/u.gg": "package util\n\nfunc Other() {}\n"})
	prog, err := buildRun(t, app, []string{gp})
	if err != nil {
		t.Fatal(err)
	}
	want := `package "app/util" is in 2 places; using the first: ` + filepath.Join(app, "util") + ", " + filepath.Join(gp, "app", "util")
	if len(prog.Warnings) != 1 || prog.Warnings[0] != want {
		t.Errorf("warnings %q\nwant %q", prog.Warnings, want)
	}
}

func TestUserPackageErrors(t *testing.T) {
	tests := []struct {
		name  string
		files map[string]string
		want  []string
	}{
		{"cycle", map[string]string{
			"main.gg": "package main\n\nimport \"app/a\"\n\nfunc main() { a.F() }\n",
			"a/a.gg":  "package a\n\nimport \"app/b\"\n\nfunc F() { b.G() }\n",
			"b/b.gg":  "package b\n\nimport \"app/a\"\n\nfunc G() { a.F() }\n",
		}, []string{filepath.Join("b", "b.gg") + ":3:8: gogo: import cycle app/a -> app/b -> app/a"}},
		{"self import", map[string]string{
			"main.gg": "package main\n\nimport \"app/a\"\n\nfunc main() { a.F() }\n",
			"a/a.gg":  "package a\n\nimport \"app/a\"\n\nfunc F() {}\n",
		}, []string{"import cycle app/a -> app/a"}},
		{"missing", map[string]string{
			"main.gg": "package main\n\nimport \"app/nope\"\n\nfunc main() {}\n",
		}, []string{"main.gg:3:8: gogo: import \"app/nope\": package not found; searched:", filepath.Join("app", "nope") + " (build root: no such directory)", filepath.Join("gp", "app", "nope") + " (search path: no such directory)"}},
		{"missing other prefix", map[string]string{
			"main.gg": "package main\n\nimport \"fmt\"\n\nfunc main() {}\n",
		}, []string{`import "fmt": package not found`, `(build root: serves only imports starting with "app/")`, filepath.Join("gp", "fmt") + " (search path: no such directory)"}},
		{"no sources", map[string]string{
			"main.gg":        "package main\n\nimport \"app/doc\"\n\nfunc main() {}\n",
			"doc/README.txt": "x",
		}, []string{"(build root: no .gg files)"}},
		{"wrong name", map[string]string{
			"main.gg":   "package main\n\nimport \"app/util\"\n\nfunc main() {}\n",
			"util/u.gg": "package utils\n",
		}, []string{filepath.Join("util", "u.gg") + ":1:9: gogo: package utils in app/util (want package util, the name of its directory)"}},
		{"main below the root", map[string]string{
			"main.gg":   "package main\n\nimport \"app/tool\"\n\nfunc main() {}\n",
			"tool/t.gg": "package main\n\nfunc main() {}\n",
		}, []string{"gogo: package main in app/tool: only the program's own directory can be package main"}},
		{"import the root", map[string]string{
			"main.gg":   "package main\n\nimport \"app/a\"\n\nfunc main() {}\n",
			"a/a.gg":    "package a\n\nimport \"app\"\n",
			"app/x.txt": "",
		}, []string{`import "app": that is the program's own directory (package main), which cannot be imported`}},
		{"bad path", map[string]string{
			"main.gg": "package main\n\nimport \"app/../x\"\n\nfunc main() {}\n",
		}, []string{`import "app/../x": bad import path`}},
		{"renamed", map[string]string{
			"main.gg":   "package main\n\nimport u \"app/util\"\n\nfunc main() { u.Sum(u.Point{}) }\n",
			"util/u.gg": utilPkg,
		}, []string{`import u "app/util" not supported`}},
		{"subset applies", map[string]string{
			"main.gg":   "package main\n\nimport \"app/util\"\n\nfunc main() { util.F() }\n",
			"util/u.gg": "package util\n\nvar g int\n\nfunc F() { println(g) }\n",
		}, []string{filepath.Join("util", "u.gg") + ":3:1: gogo: global variable not supported"}},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			work := t.TempDir()
			app := writeTree(t, filepath.Join(work, "app"), tc.files)
			_, err := buildRun(t, app, []string{filepath.Join(work, "gp")})
			if err == nil {
				t.Fatal("built, want an error")
			}
			for _, w := range tc.want {
				if !strings.Contains(err.Error(), w) {
					t.Errorf("error %q\nlacks %q", err, w)
				}
			}
		})
	}
}

// Requirements are inferred from user packages too, each with its own
// //gogo:optional.
func TestUserPackageRequires(t *testing.T) {
	work := t.TempDir()
	app := writeTree(t, filepath.Join(work, "app"), map[string]string{
		"main.gg":     "package main\n\nimport (\n\t\"app/radio\"\n\t\"app/snd\"\n)\n\nfunc main() { println(radio.Up(), snd.Rec()) }\n",
		"radio/r.gg":  "package radio\n\nimport \"gogo/port\"\n\nfunc Up() bool { _, ok := port.Open(\"udp\", 1); return ok }\n",
		"snd/snd.gg":  "// Package snd records when it can.\n//\n//gogo:optional mic\npackage snd\n\nimport \"gogo/port\"\n\nfunc Rec() bool { _, ok := port.Open(\"mic\", 1); return ok }\n",
		"snd/more.gg": "package snd\n\nimport \"gogo/port\"\n\nfunc IR() bool { _, ok := port.Open(\"ir_tx\", 1); return ok }\n",
	})
	prog, err := Config{}.Load(app)
	if err != nil {
		t.Fatal(err)
	}
	names, _ := Requires(prog)
	if got := strings.Join(names, " "); got != "ir_tx udp" {
		t.Errorf("Requires = %q, want \"ir_tx udp\"", got)
	}
}

// LoadFile compiles one file; the build root is its directory, whose
// other .gg files are not read, but whose packages are found.
func TestLoadFile(t *testing.T) {
	work := t.TempDir()
	dir := writeTree(t, filepath.Join(work, "scripts"), map[string]string{
		"hello.gg":  "package main\n\nfunc main() { println(\"hi\") }\n",
		"other.gg":  "package main\n\nfunc main() { println(\"other\") }\n",
		"broken.gg": "package nope\n",
		"sum.gg":    "package main\n\nimport \"scripts/util\"\n\nfunc main() { println(util.Sum(util.Point{X: 1})) }\n",
		"util/u.gg": utilPkg,
	})
	one, err := Config{}.LoadFile(filepath.Join(dir, "hello.gg"))
	if err != nil {
		t.Fatal(err)
	}
	if len(one.Files) != 1 {
		t.Errorf("%d files compiled, want 1", len(one.Files))
	}
	if _, err := (Config{}).LoadFile(filepath.Join(dir, "sum.gg")); err != nil {
		t.Errorf("sibling package: %v", err)
	}
	if _, err := (Config{}).Load(dir); err == nil {
		t.Error("the directory built, want an error (two func main, package nope)")
	}
	// A single file compiles to the same image as its directory alone.
	solo := writeTree(t, filepath.Join(work, "solo"), map[string]string{"main.gg": "package main\n\nfunc main() { println(\"hi\") }\n"})
	a, _ := Compile(one)
	sp, err := Config{}.Load(solo)
	if err != nil {
		t.Fatal(err)
	}
	b, _ := Compile(sp)
	if string(a) != string(b) {
		t.Error("single-file image differs from the directory's")
	}
}

// A first line starting with #! is a blank line: the file builds and its
// errors keep their line numbers.
func TestShebang(t *testing.T) {
	work := t.TempDir()
	dir := writeTree(t, filepath.Join(work, "s"), map[string]string{
		"ok.gg":  "#!/usr/bin/env -S gogo run\npackage main\n\nfunc main() { println(\"hi\") }\n",
		"bad.gg": "#!/usr/bin/env -S gogo run\npackage main\n\nfunc main() {\n\tm := map[int]int{}\n\t_ = m\n}\n",
		"typ.gg": "#!/usr/bin/env -S gogo run\npackage main\n\nfunc main() { undefined() }\n",
		"pkg.gg": "#!/usr/bin/env -S gogo run\npackage main\n\nimport \"s/util\"\n\nfunc main() { util.Sum(util.Point{}) }\n",
		// a package file may start with #! too
		"util/u.gg": "#!x\n" + utilPkg,
		"only.gg":   "#!/bin/sh",
	})
	shebang, err := Config{}.LoadFile(filepath.Join(dir, "ok.gg"))
	if err != nil {
		t.Fatal(err)
	}
	plain := writeTree(t, filepath.Join(work, "plain"), map[string]string{"main.gg": "\npackage main\n\nfunc main() { println(\"hi\") }\n"})
	pp, err := Config{}.Load(plain)
	if err != nil {
		t.Fatal(err)
	}
	a, _ := Compile(shebang)
	b, _ := Compile(pp)
	if string(a) != string(b) {
		t.Error("the #! file compiles to another image")
	}
	prog, err := Config{}.LoadFile(filepath.Join(dir, "bad.gg"))
	if err != nil {
		t.Fatal(err)
	}
	if errs := Check(prog); len(errs) == 0 || !strings.Contains(errs[0].Error(), "bad.gg:5:7: gogo: map not supported") {
		t.Errorf("errors %v, want bad.gg:5:7", errs)
	}
	if _, err := (Config{}).LoadFile(filepath.Join(dir, "typ.gg")); err == nil || !strings.Contains(err.Error(), "typ.gg:4:15: undefined: undefined") {
		t.Errorf("error %v, want typ.gg:4:15", err)
	}
	if _, err := (Config{}).LoadFile(filepath.Join(dir, "pkg.gg")); err != nil {
		t.Errorf("package with #!: %v", err)
	}
	if _, err := (Config{}).LoadFile(filepath.Join(dir, "only.gg")); err == nil || !strings.Contains(err.Error(), "expected 'package'") {
		t.Errorf("a file of only #!: %v", err)
	}
	// Only a first line: #! later is a syntax error.
	late := writeTree(t, filepath.Join(work, "late"), map[string]string{"main.gg": "package main\n#!x\nfunc main() {}\n"})
	if _, err := (Config{}).Load(late); err == nil {
		t.Error("#! on line 2 accepted")
	}
}

func TestSearchPathDefault(t *testing.T) {
	sep := string(os.PathListSeparator)
	home := filepath.Join(string(filepath.Separator)+"home", "u")
	a, b := filepath.Join(home, "a"), filepath.Join(home, "b")
	tests := []struct {
		name string
		gp   string
		set  bool
		home string
		want []string
	}{
		{"unset", "", false, home, []string{filepath.Join(home, "gogo")}},
		{"unset no home", "", false, "", nil},
		{"set empty", "", true, home, nil},
		{"two", a + sep + b, true, home, []string{a, b}},
		{"empty elements", sep + a + sep + sep + b + sep, true, home, []string{a, b}},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			got := searchPath(tc.gp, tc.set, tc.home)
			if strings.Join(got, "|") != strings.Join(tc.want, "|") {
				t.Errorf("searchPath = %q, want %q", got, tc.want)
			}
		})
	}
	if got := SplitPath("rel"); len(got) != 1 || !filepath.IsAbs(got[0]) {
		t.Errorf("SplitPath(rel) = %q, want an absolute path", got)
	}
}

// A build root inside a search path directory (projects kept in ~/gogo)
// finds its packages in one place, not two.
func TestRootInsideSearchPath(t *testing.T) {
	gp := t.TempDir()
	app := writeTree(t, filepath.Join(gp, "app"), map[string]string{
		"main.gg":   "package main\n\nimport \"app/util\"\n\nfunc main() { util.Sum(util.Point{}) }\n",
		"util/u.gg": utilPkg,
	})
	link := filepath.Join(t.TempDir(), "gp-link")
	if err := os.Symlink(gp, link); err != nil {
		t.Fatal(err)
	}
	// The default search path, $HOME/gogo, with the project in ~/gogo/app.
	home := t.TempDir()
	if err := os.Symlink(gp, filepath.Join(home, "gogo")); err != nil {
		t.Fatal(err)
	}
	def := searchPath("", false, home)
	for _, path := range [][]string{{gp}, {link}, {gp, link}, def} {
		prog, err := buildRun(t, app, path)
		if err != nil {
			t.Fatal(err)
		}
		if len(prog.Warnings) != 0 {
			t.Errorf("search path %q: warnings %q", path, prog.Warnings)
		}
	}
	// Built from inside the search path directory as the cwd too.
	wd, _ := os.Getwd()
	defer os.Chdir(wd)
	if err := os.Chdir(gp); err != nil {
		t.Fatal(err)
	}
	prog, err := buildRun(t, "app", []string{gp})
	if err != nil {
		t.Fatal(err)
	}
	if len(prog.Warnings) != 0 {
		t.Errorf("warnings %q", prog.Warnings)
	}
}

// caseInsensitive reports whether the file system under dir ignores case.
func caseInsensitive(t *testing.T, dir string) bool {
	p := filepath.Join(dir, "CaseProbe")
	if err := os.WriteFile(p, nil, 0o644); err != nil {
		t.Fatal(err)
	}
	defer os.Remove(p)
	_, err := os.Stat(filepath.Join(dir, "caseprobe"))
	return err == nil
}

// An import path must match the directory names on disk, letter case
// included, so a program that builds on macOS builds on Linux.
func TestImportCase(t *testing.T) {
	work := t.TempDir()
	app := writeTree(t, filepath.Join(work, "app"), map[string]string{
		"main.gg":   "package main\n\nimport \"app/util\"\n\nfunc main() { util.Sum(util.Point{}) }\n",
		"Util/u.gg": utilPkg,
	})
	_, err := buildRun(t, app, nil)
	want := "package not found"
	if caseInsensitive(t, work) {
		want = "the directory is " + filepath.Join(app, "Util") + " on disk: import paths must match directory names, letter case included"
	}
	if err == nil || !strings.Contains(err.Error(), want) {
		t.Errorf("error %v, want %q", err, want)
	}
	// A root typed in another case still serves imports in its real case.
	if caseInsensitive(t, work) {
		ok := writeTree(t, filepath.Join(work, "proj"), map[string]string{
			"main.gg":   "package main\n\nimport \"proj/util\"\n\nfunc main() { util.Sum(util.Point{}) }\n",
			"util/u.gg": utilPkg,
		})
		if _, err := buildRun(t, filepath.Join(filepath.Dir(ok), "PROJ"), nil); err != nil {
			t.Errorf("root typed PROJ: %v", err)
		}
	}
}

func TestUserPackageMoreErrors(t *testing.T) {
	work := t.TempDir()
	tests := []struct {
		name  string
		files map[string]string
		want  string
	}{
		{"stdlib without prefix", map[string]string{
			"main.gg": "package main\n\nimport \"strings\"\n\nfunc main() {}\n",
		}, `(did you mean "gogo/strings"?)`},
		{"dash in name", map[string]string{
			"main.gg":     "package main\n\nimport \"app/my-lib\"\n\nfunc main() {}\n",
			"my-lib/l.gg": "package mylib\n",
		}, `import "app/my-lib": "my-lib" cannot be a package name`},
		{"same directory twice", map[string]string{
			"main.gg":   "package main\n\nimport (\n\t\"app/util\"\n\t\"app/x/util\"\n)\n\nfunc main() { util.Sum(util.Point{}) }\n",
			"util/u.gg": utilPkg,
		}, `already imported as "app/util": use one import path`},
	}
	for i, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			app := writeTree(t, filepath.Join(work, strconv.Itoa(i), "app"), tc.files)
			if tc.name == "same directory twice" {
				if err := os.Symlink(app, filepath.Join(app, "x")); err != nil {
					t.Fatal(err)
				}
			}
			_, err := buildRun(t, app, nil)
			if err == nil || !strings.Contains(err.Error(), tc.want) {
				t.Errorf("error %v, want %q", err, tc.want)
			}
		})
	}
}

// Functions of user packages are named by import path in the image,
// libraries by package name.
func TestUserPackageNames(t *testing.T) {
	work := t.TempDir()
	app := writeTree(t, filepath.Join(work, "app"), map[string]string{
		"main.gg":     "package main\n\nimport (\n\t\"app/a/util\"\n\t\"gogo/strings\"\n)\n\nfunc main() { util.Mk(); println(strings.TrimSpace(\" \")) }\n",
		"a/util/u.gg": "package util\n\nimport \"app/b/util\"\n\nfunc Mk() { f := func() { util.Mk() }; f() }\n",
		"b/util/u.gg": "package util\n\nfunc Mk() {}\n",
	})
	prog, err := buildRun(t, app, nil)
	if err != nil {
		t.Fatal(err)
	}
	img, err := Compile(prog)
	if err != nil {
		t.Fatal(err)
	}
	text, err := Disassemble(img)
	if err != nil {
		t.Fatal(err)
	}
	for _, w := range []string{"func app/a/util.Mk ", "func app/b/util.Mk ", "func app/a/util.Mk.func1 ", "func strings.TrimSpace "} {
		if !strings.Contains(text, w) {
			t.Errorf("disassembly lacks %q", w)
		}
	}
}
