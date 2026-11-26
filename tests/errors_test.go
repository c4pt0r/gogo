package tests

import (
	"go/ast"
	"go/constant"
	"go/parser"
	"go/token"
	"go/types"
	"os"
	"regexp"
	"slices"
	"strconv"
	"strings"
	"testing"
)

// TestErrorCodes keeps runtime/include/gogo_errors.h, the C side's one
// list of the shared error codes, equal to gogo/errors (lib/errors): every
// errors.X constant must be GOGO_E_<X in capitals> with the same value and
// every GOGO_E_* an errors constant. The shared codes stay in 1..30, below
// the reserved 64..99 and the per-package blocks from 100.
func TestErrorCodes(t *testing.T) {
	fset := token.NewFileSet()
	f, err := parser.ParseFile(fset, "../lib/errors/errors.gg", nil, 0)
	if err != nil {
		t.Fatal(err)
	}
	conf := types.Config{}
	pkg, err := conf.Check("gogo/errors", fset, []*ast.File{f}, nil)
	if err != nil {
		t.Fatal(err)
	}
	gogo := map[string]int64{}
	for _, name := range pkg.Scope().Names() {
		if k, ok := pkg.Scope().Lookup(name).(*types.Const); ok {
			gogo[name], _ = constant.Int64Val(k.Val())
		}
	}
	if len(gogo) == 0 {
		t.Fatal("no constants in lib/errors/errors.gg")
	}

	hdr, err := os.ReadFile("../runtime/include/gogo_errors.h")
	if err != nil {
		t.Fatal(err)
	}
	c := map[string]int64{}
	for _, m := range regexp.MustCompile(`\bGOGO_E_([A-Z0-9]+)\s*=\s*(\d+)`).FindAllSubmatch(hdr, -1) {
		c[string(m[1])], _ = strconv.ParseInt(string(m[2]), 10, 64)
	}

	seen := map[int64]string{}
	for name, v := range gogo {
		if prev, dup := seen[v]; dup {
			t.Errorf("errors.%s and errors.%s are both %d", prev, name, v)
		}
		seen[v] = name
		if name != "OK" && (v < 1 || v > 30) {
			t.Errorf("errors.%s = %d: a shared code is in 1..30", name, v)
		}
		cv, ok := c[strings.ToUpper(name)]
		if !ok {
			t.Errorf("errors.%s has no GOGO_E_%s in gogo_errors.h", name, strings.ToUpper(name))
		} else if cv != v {
			t.Errorf("errors.%s = %d, GOGO_E_%s = %d", name, v, strings.ToUpper(name), cv)
		}
	}
	var extra []string
	for cname := range c {
		found := false
		for name := range gogo {
			found = found || strings.ToUpper(name) == cname
		}
		if !found {
			extra = append(extra, "GOGO_E_"+cname)
		}
	}
	slices.Sort(extra)
	if len(extra) > 0 {
		t.Errorf("not in gogo/errors: %s", strings.Join(extra, ", "))
	}
}
