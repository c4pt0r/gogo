package main

import (
	"bytes"
	"encoding/json"
	"strings"
	"testing"
)

const blob = Repo + "/blob/main/"
const tree = Repo + "/tree/main/"

func TestRewrite(t *testing.T) {
	isDir := func(p string) bool { return p == "examples/embed-c" || p == "docs/book/examples" }
	for _, c := range []struct{ in, want string }{
		// chapters, anchors and URLs are left to mdBook
		{"[a](04-actors.md)", "[a](04-actors.md)"},
		{"[a](05-x.md#links)", "[a](05-x.md#links)"},
		{"[a](README.md)", "[a](README.md)"},
		{"[a](#section)", "[a](#section)"},
		{"[a](https://example.com/x.md)", "[a](https://example.com/x.md)"},
		{"[a](mailto:x@example.com)", "[a](mailto:x@example.com)"},
		{"[a](/abs/path)", "[a](/abs/path)"},
		// repository files: blob; directories: tree
		{"[k](../known-issues.md)", "[k](" + blob + "docs/known-issues.md)"},
		{"[k](../known-issues.md#tls)", "[k](" + blob + "docs/known-issues.md#tls)"},
		{"[h](../../runtime/include/gogo.h)", "[h](" + blob + "runtime/include/gogo.h)"},
		{"[e](../../examples/embed-c/)", "[e](" + tree + "examples/embed-c)"},
		{"[e](../../examples/embed-c)", "[e](" + tree + "examples/embed-c)"},
		{"[`examples/`](examples/)", "[`examples/`](" + tree + "docs/book/examples)"},
		{"[x](examples/02-hello/main.gg)", "[x](" + blob + "docs/book/examples/02-hello/main.gg)"},
		// a title is kept
		{`[k](../design.md "Design")`, `[k](` + blob + `docs/design.md "Design")`},
		// two links in a line, text around them
		{"see [a](../a.md) and [b](04-b.md).", "see [a](" + blob + "docs/a.md) and [b](04-b.md)."},
		// outside the repository: unchanged
		{"[x](../../../elsewhere.md)", "[x](../../../elsewhere.md)"},
		// inline code is not a link
		{"`f[T](a, b T)` and [k](../k.md)", "`f[T](a, b T)` and [k](" + blob + "docs/k.md)"},
		{"``x ` [a](b) ``", "``x ` [a](b) ``"},
		// an unclosed target or backtick is left as it is
		{"[a](../x.md", "[a](../x.md"},
		{"a ` b [k](../k.md)", "a ` b [k](" + blob + "docs/k.md)"},
		// parentheses inside a target nest
		{"[w](../a_(b).md)", "[w](" + blob + "docs/a_(b).md)"},
	} {
		if got := Rewrite(c.in, ".", isDir); got != c.want {
			t.Errorf("Rewrite(%q)\n got %q\nwant %q", c.in, got, c.want)
		}
	}
}

func TestRewriteSkipsFences(t *testing.T) {
	in := "a [k](../k.md)\n```go\nfunc Max[T int](a, b T) T {}\n```\n  ```sh\n  [x](../y)\n  ```\n````\n```\n[z](../z)\n````\nb [k](../k.md)\n"
	want := strings.ReplaceAll(in, "(../k.md)", "("+blob+"docs/k.md)")
	if got := Rewrite(in, ".", nil); got != want {
		t.Errorf("got\n%s\nwant\n%s", got, want)
	}
}

func TestRewriteSubdir(t *testing.T) {
	if got, want := Rewrite("[a](../02-x.md) [b](main.gg)", "examples/02-hello", nil),
		"[a]("+blob+"docs/book/examples/02-x.md) [b]("+blob+"docs/book/examples/02-hello/main.gg)"; got != want {
		t.Errorf("got %q want %q", got, want)
	}
}

func TestHasScheme(t *testing.T) {
	for s, want := range map[string]bool{
		"https://x": true, "mailto:a": true, "git+ssh:x": true,
		"../a.md": false, "a.md": false, ":x": false, "a/b:c": false, "1http:x": false,
	} {
		if got := hasScheme(s); got != want {
			t.Errorf("hasScheme(%q) = %v", s, got)
		}
	}
}

// TestRun feeds run the JSON mdBook sends and checks the book that comes
// back: chapter content rewritten, nested chapters too, the rest unchanged.
func TestRun(t *testing.T) {
	in := `[{"root":"/nowhere","config":{},"renderer":"html","mdbook_version":"0.5.4"},
{"items":[{"Chapter":{"name":"A","content":"[k](../known-issues.md)","number":[1],
"sub_items":[{"Chapter":{"name":"B","content":"[r](../../runtime/x.c)","path":"sub/b.md","sub_items":[]}}],
"path":"01-a.md","source_path":"01-a.md","parent_names":[]}},"Separator",
{"Chapter":{"name":"Draft","content":"[k](../k.md)","path":null,"sub_items":[]}}]}]`
	var out bytes.Buffer
	if err := run(strings.NewReader(in), &out); err != nil {
		t.Fatal(err)
	}
	var book struct {
		Items []json.RawMessage `json:"items"`
	}
	if err := json.Unmarshal(out.Bytes(), &book); err != nil {
		t.Fatal(err)
	}
	if len(book.Items) != 3 {
		t.Fatalf("items: %s", out.String())
	}
	s := out.String()
	for _, want := range []string{
		`"content":"[k](` + blob + `docs/known-issues.md)"`,
		`"content":"[r](` + blob + `docs/runtime/x.c)"`, // sub/b.md: docs/book/sub/../../runtime
		`"content":"[k](../k.md)"`,                      // a draft chapter has no path
		`"number":[1]`, `"Separator"`,
	} {
		if !strings.Contains(s, want) {
			t.Errorf("output lacks %s:\n%s", want, s)
		}
	}
}

func TestRunBadInput(t *testing.T) {
	for _, in := range []string{"", "{}", "[1]", `[{}, {"items": [}]`} {
		if err := run(strings.NewReader(in), &bytes.Buffer{}); err == nil {
			t.Errorf("run(%q): no error", in)
		}
	}
}
