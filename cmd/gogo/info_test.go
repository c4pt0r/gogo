package main

import (
	"bytes"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"gogotools/compiler"
)

func TestInfoImage(t *testing.T) {
	dir := t.TempDir()
	b := compiler.NewImageBuilder()
	work := b.AddFunc("Work", 1, 2)
	b.SetFuncCode(work, []uint32{uint32(compiler.OpRet)})
	f := b.AddFunc("main", 0, 1)
	b.SetFuncCode(f, []uint32{uint32(compiler.OpRet)})
	b.SetEntry(f)
	b.AddExport("Work", work)
	b.AddExport("main", f)
	b.SetRequires([]string{"/sd", "kbd"})
	img, err := b.Bytes()
	if err != nil {
		t.Fatal(err)
	}
	p := filepath.Join(dir, "m.ggb")
	if err := os.WriteFile(p, img, 0o644); err != nil {
		t.Fatal(err)
	}
	var out bytes.Buffer
	if err := infoCmd([]string{p}, &out); err != nil {
		t.Fatal(err)
	}
	want := fmt.Sprintf(`format     %d.0
size       %d bytes: 2 functions, 0 types, 2 code words, 12 constant words
requires   /sd, kbd
entries    Work, main (boot)
`, compiler.ImageMajor, len(img))
	if out.String() != want {
		t.Errorf("got\n%s\nwant\n%s", out.String(), want)
	}

	// A truncated or foreign file is an error naming it.
	if err := os.WriteFile(p, img[:len(img)-4], 0o644); err != nil {
		t.Fatal(err)
	}
	if err := infoCmd([]string{p}, &out); err == nil || !strings.Contains(err.Error(), "m.ggb: truncated") {
		t.Errorf("truncated: %v", err)
	}
	if err := infoCmd([]string{"a", "b"}, &out); err == nil || !strings.Contains(err.Error(), "usage: gogo info") {
		t.Errorf("two files: %v", err)
	}
}

// From source, info also lists the soft (//gogo:optional) drivers, which
// the image does not record.
func TestInfoSource(t *testing.T) {
	dir := t.TempDir()
	src := `// Program p probes the keyboard.
//
//gogo:optional kbd
package main

import "gogo/port"

// Greet is an entry.
func Greet(arg any) {}

func main() {
	_, _ = port.Open("kbd", 1)
	_, _ = port.Open("udp", 1)
}
`
	if err := os.WriteFile(filepath.Join(dir, "main.gg"), []byte(src), 0o644); err != nil {
		t.Fatal(err)
	}
	var out bytes.Buffer
	if err := infoCmd([]string{"-path", "", dir}, &out); err != nil {
		t.Fatal(err)
	}
	for _, line := range []string{"requires   udp\n", "optional   kbd (main)\n", "entries    Greet, main (boot)\n"} {
		if !strings.Contains(out.String(), line) {
			t.Errorf("no %q in\n%s", line, out.String())
		}
	}
}
