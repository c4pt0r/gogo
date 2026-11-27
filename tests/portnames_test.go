package tests

import (
	"io/fs"
	"os"
	"path/filepath"
	"regexp"
	"slices"
	"strings"
	"testing"
)

// TestPortNames keeps runtime/vm/port_names.h, the one list of the
// runtime's port driver names (reserved as host port names on every
// board), in step with the names the libraries open and the firmware
// provides: every PORT_NAME_* must be a name some gogo/... package passes
// to port.Open or a cmd/gogo/firmware.go provides (or fwBase) entry, and
// every such name must be a PORT_NAME_*. No runtime driver may spell its
// name as a literal instead of using the header.
func TestPortNames(t *testing.T) {
	hdr, err := os.ReadFile("../runtime/vm/port_names.h")
	if err != nil {
		t.Fatal(err)
	}
	// The runtime's names never have a '.': a dotted name is a third
	// party's (a board port's vendor prefix, book 22.4), so the two can
	// never collide.
	for _, m := range regexp.MustCompile(`(?m)^#define PORT_\w+\s+"([^"]*)"`).FindAllSubmatch(hdr, -1) {
		if strings.Contains(string(m[1]), ".") {
			t.Errorf("runtime/vm/port_names.h: %q has a '.', which only third-party ports may have", m[1])
		}
	}
	listed := map[string]bool{}
	for _, m := range regexp.MustCompile(`(?m)^#define PORT_NAME_\w+\s+"([a-z0-9_]+)"`).FindAllSubmatch(hdr, -1) {
		listed[string(m[1])] = true
	}
	// Requirement names that are not drivers (PORT_REQ_*: "float"), which
	// the firmware provides too.
	reqs := map[string]bool{}
	for _, m := range regexp.MustCompile(`(?m)^#define PORT_REQ_\w+\s+"([a-z0-9_]+)"`).FindAllSubmatch(hdr, -1) {
		reqs[string(m[1])] = true
	}
	if len(listed) == 0 {
		t.Fatal("no PORT_NAME_* in runtime/vm/port_names.h")
	}

	used := map[string]string{} // name -> where it comes from
	open := regexp.MustCompile(`port\.Open\("([^"]+)"`)
	err = filepath.WalkDir("../lib", func(p string, d fs.DirEntry, err error) error {
		if err != nil || d.IsDir() || !strings.HasSuffix(p, ".gg") {
			return err
		}
		src, err := os.ReadFile(p)
		if err != nil {
			return err
		}
		for _, m := range open.FindAllSubmatch(src, -1) {
			used[string(m[1])] = p
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	fw, err := os.ReadFile("../cmd/gogo/firmware.go")
	if err != nil {
		t.Fatal(err)
	}
	lists := regexp.MustCompile(`(?:provides: |fwBase = )\[\]string\{([^}]*)\}`).FindAllSubmatch(fw, -1)
	if len(lists) == 0 {
		t.Fatal("no provides lists in cmd/gogo/firmware.go")
	}
	for _, l := range lists {
		for _, m := range regexp.MustCompile(`"([^"]+)"`).FindAllSubmatch(l[1], -1) {
			if n := string(m[1]); !strings.HasPrefix(n, "/") && !reqs[n] { // mounts and PORT_REQ_* are not ports
				used[n] = "cmd/gogo/firmware.go"
			}
		}
	}

	var missing, extra []string
	for n, where := range used {
		if !listed[n] {
			missing = append(missing, n+" ("+where+")")
		}
	}
	for n := range listed {
		if _, ok := used[n]; !ok {
			extra = append(extra, n)
		}
	}
	slices.Sort(missing)
	slices.Sort(extra)
	if len(missing) > 0 {
		t.Errorf("port names not in runtime/vm/port_names.h: %s", strings.Join(missing, ", "))
	}
	if len(extra) > 0 {
		t.Errorf("PORT_NAME_* no library opens and no firmware provides: %s", strings.Join(extra, ", "))
	}

	// Drivers take their names from the header: no literal name in a
	// PortDriver initializer outside the tests.
	lit := regexp.MustCompile(`(?m)^\s*\{?\s*"[a-z0-9_]+",\s*(?:TYPE_|UINT32_MAX)`)
	for _, dir := range []string{"../runtime", "../platform"} {
		err := filepath.WalkDir(dir, func(p string, d fs.DirEntry, err error) error {
			if err != nil {
				return err
			}
			if d.IsDir() {
				if d.Name() == "test" || d.Name() == "build" || strings.HasPrefix(d.Name(), "managed_components") {
					return filepath.SkipDir
				}
				return nil
			}
			if !strings.HasSuffix(p, ".c") && !strings.HasSuffix(p, ".h") {
				return nil
			}
			src, err := os.ReadFile(p)
			if err != nil {
				return err
			}
			if m := lit.Find(src); m != nil {
				t.Errorf("%s: a driver name as a literal (%q): use runtime/vm/port_names.h", p, strings.TrimSpace(string(m)))
			}
			return nil
		})
		if err != nil {
			t.Fatal(err)
		}
	}
}
