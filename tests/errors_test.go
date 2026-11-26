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
// every GOGO_E_* an errors constant. The shared codes stay in 1..35, so
// that actor.Failed (64) + a code is a library exit reason below the user
// codes from 100 (actor.Failed is checked here too).
// maxShared is the largest shared code: actor.Failed + maxShared must
// stay below 100, the first user exit code.
const maxShared = 35

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
	for _, m := range regexp.MustCompile(`\bGOGO_E_([A-Z0-9]+)\s*=\s*(0[xX][0-9a-fA-F]+|\d+)`).FindAllSubmatch(hdr, -1) {
		v, err := strconv.ParseInt(string(m[2]), 0, 64)
		if err != nil {
			t.Errorf("GOGO_E_%s = %s: %v", m[1], m[2], err)
		}
		c[string(m[1])] = v
	}
	if m := regexp.MustCompile(`#define GOGO_E_SHARED_MAX\s+(\d+)`).FindSubmatch(hdr); m == nil || string(m[1]) != strconv.Itoa(maxShared) {
		t.Errorf("gogo_errors.h: GOGO_E_SHARED_MAX is not %d", maxShared)
	}
	if n := len(regexp.MustCompile(`\bGOGO_E_[A-Z0-9]+\s*=`).FindAll(hdr, -1)); n != len(c) {
		t.Errorf("gogo_errors.h has %d GOGO_E_* values, %d of them decimal or hex numbers", n, len(c))
	}

	seen := map[int64]string{}
	for name, v := range gogo {
		if prev, dup := seen[v]; dup {
			t.Errorf("errors.%s and errors.%s are both %d", prev, name, v)
		}
		seen[v] = name
		if name != "OK" && (v < 1 || v > maxShared) {
			t.Errorf("errors.%s = %d: a shared code is in 1..%d", name, v, maxShared)
		}
		cv, ok := c[strings.ToUpper(name)]
		if !ok {
			t.Errorf("errors.%s has no GOGO_E_%s in gogo_errors.h", name, strings.ToUpper(name))
		} else if cv != v {
			t.Errorf("errors.%s = %d, GOGO_E_%s = %d", name, v, strings.ToUpper(name), cv)
		}
	}
	var last int64
	for _, v := range gogo {
		last = max(last, v)
	}
	if m := regexp.MustCompile(`#define GOGO_E_SHARED_LAST\s+(\d+)`).FindSubmatch(hdr); m == nil || string(m[1]) != strconv.FormatInt(last, 10) {
		t.Errorf("gogo_errors.h: GOGO_E_SHARED_LAST is not %d, the highest code in gogo/errors", last)
	}
	failed := actorFailed(t)
	if failed+maxShared >= 100 {
		t.Errorf("actor.Failed %d + %d reaches the user exit codes", failed, maxShared)
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

// actorFailed is gogo/actor's Failed constant, read from lib/actor.
func actorFailed(t *testing.T) int64 {
	t.Helper()
	src, err := os.ReadFile("../lib/actor/actor.gg")
	if err != nil {
		t.Fatal(err)
	}
	m := regexp.MustCompile(`(?m)^\s*Failed\s*=\s*(\d+)`).FindSubmatch(src)
	if m == nil {
		t.Fatal("no Failed constant in lib/actor/actor.gg")
	}
	v, _ := strconv.ParseInt(string(m[1]), 10, 64)
	return v
}

// TestErrorTexts keeps errors.Code.Error's texts (lib/errors) equal to
// gogo_strerror's (runtime/embed/gogo.c, code_text), code by code.
func TestErrorTexts(t *testing.T) {
	src, err := os.ReadFile("../runtime/embed/gogo.c")
	if err != nil {
		t.Fatal(err)
	}
	m := regexp.MustCompile(`(?s)code_text\[\] = \{(.*?)\};`).FindSubmatch(src)
	if m == nil {
		t.Fatal("code_text not found in runtime/embed/gogo.c")
	}
	var c []string
	for _, s := range regexp.MustCompile(`"([^"]*)"`).FindAllSubmatch(m[1], -1) {
		c = append(c, string(s[1]))
	}
	gg, err := os.ReadFile("../lib/errors/errors.gg")
	if err != nil {
		t.Fatal(err)
	}
	fset := token.NewFileSet()
	f, err := parser.ParseFile(fset, "errors.gg", gg, 0)
	if err != nil {
		t.Fatal(err)
	}
	pkg, err := (&types.Config{}).Check("gogo/errors", fset, []*ast.File{f}, nil)
	if err != nil {
		t.Fatal(err)
	}
	texts := map[int64]string{}
	ast.Inspect(f, func(n ast.Node) bool {
		cc, ok := n.(*ast.CaseClause)
		if !ok || len(cc.List) != 1 || len(cc.Body) != 1 {
			return true
		}
		id, ok := cc.List[0].(*ast.Ident)
		ret, ok2 := cc.Body[0].(*ast.ReturnStmt)
		if !ok || !ok2 || len(ret.Results) != 1 {
			return true
		}
		k, ok := pkg.Scope().Lookup(id.Name).(*types.Const)
		lit, ok2 := ret.Results[0].(*ast.BasicLit)
		if ok && ok2 {
			v, _ := constant.Int64Val(k.Val())
			texts[v], _ = strconv.Unquote(lit.Value)
		}
		return true
	})
	if len(texts) != len(c) {
		t.Errorf("Code.Error has %d texts, gogo_strerror %d", len(texts), len(c))
	}
	for i, s := range c {
		if texts[int64(i)] != s {
			t.Errorf("code %d: Code.Error %q, gogo_strerror %q", i, texts[int64(i)], s)
		}
	}
}
