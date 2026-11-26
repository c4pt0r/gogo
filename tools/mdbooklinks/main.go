// Command mdbooklinks is an mdBook preprocessor (docs/book/book.toml) that
// points the book's links to repository files outside the book at GitHub.
//
// The book links to files such as ../known-issues.md, ../../runtime/include/
// gogo.h and examples/ with relative paths, which GitHub follows but which
// do not exist on the website mdBook builds (make site). For each chapter,
// every inline link [text](target) outside code whose target is a relative
// path that is not a chapter (a .md file directly in docs/book) becomes
//
//	https://github.com/c4pt0r/gogo/blob/main/<repo path>[#fragment]
//
// or .../tree/main/<repo path> for a directory. Chapter links (04-actors.md,
// 05-x.md#anchor), anchors (#x), absolute URLs and links inside fenced or
// inline code are left alone; mdBook turns the chapter links into .html.
//
// mdBook calls it as `mdbooklinks supports <renderer>` (every renderer is
// supported) and then with the JSON [context, book] on stdin, expecting the
// book back on stdout.
package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path"
	"path/filepath"
	"strings"
)

// Repo is where repository links point; bookDir is the book's directory in
// the repository.
const (
	Repo    = "https://github.com/c4pt0r/gogo"
	bookDir = "docs/book"
)

func main() {
	if len(os.Args) > 1 && os.Args[1] == "supports" {
		return
	}
	if err := run(os.Stdin, os.Stdout); err != nil {
		fmt.Fprintln(os.Stderr, "mdbooklinks:", err)
		os.Exit(1)
	}
}

func run(in io.Reader, out io.Writer) error {
	var pair []json.RawMessage
	if err := json.NewDecoder(in).Decode(&pair); err != nil {
		return err
	}
	if len(pair) != 2 {
		return fmt.Errorf("want [context, book] on stdin, got %d values", len(pair))
	}
	var ctx struct {
		Root string `json:"root"`
	}
	if err := json.Unmarshal(pair[0], &ctx); err != nil {
		return err
	}
	dec := json.NewDecoder(bytes.NewReader(pair[1]))
	dec.UseNumber()
	var book any
	if err := dec.Decode(&book); err != nil {
		return err
	}
	isDir := func(repoPath string) bool {
		if ctx.Root == "" {
			return false
		}
		rel, err := filepath.Rel(bookDir, repoPath)
		if err != nil {
			return false
		}
		st, err := os.Stat(filepath.Join(ctx.Root, rel))
		return err == nil && st.IsDir()
	}
	walk(book, isDir)
	return json.NewEncoder(out).Encode(book)
}

// walk rewrites the content of every chapter in v: an object with a string
// "content" and a string "path" (the chapter file, relative to the book).
func walk(v any, isDir func(string) bool) {
	switch v := v.(type) {
	case map[string]any:
		content, ok1 := v["content"].(string)
		p, ok2 := v["path"].(string)
		if ok1 && ok2 {
			v["content"] = Rewrite(content, path.Dir(filepath.ToSlash(p)), isDir)
		}
		for _, x := range v {
			walk(x, isDir)
		}
	case []any:
		for _, x := range v {
			walk(x, isDir)
		}
	}
}

// Rewrite rewrites the links in markdown md, a chapter in the directory dir
// relative to the book ("." for the book's top level). isDir reports whether
// a repository path is a directory (nil: only a trailing / says so).
func Rewrite(md, dir string, isDir func(string) bool) string {
	var b strings.Builder
	fence := "" // the open fence ("```" or longer), or ""
	for _, line := range strings.SplitAfter(md, "\n") {
		t := strings.TrimLeft(line, " \t")
		if fence != "" {
			if strings.HasPrefix(t, fence) && strings.TrimSpace(strings.TrimLeft(t, "`~")) == "" {
				fence = ""
			}
			b.WriteString(line)
			continue
		}
		if f := fenceOf(t); f != "" {
			fence = f
			b.WriteString(line)
			continue
		}
		b.WriteString(rewriteLine(line, dir, isDir))
	}
	return b.String()
}

// fenceOf returns the fence a line opens (its leading ``` or ~~~, three or
// more), or "".
func fenceOf(t string) string {
	for _, c := range []byte{'`', '~'} {
		n := 0
		for n < len(t) && t[n] == c {
			n++
		}
		if n >= 3 {
			return t[:n]
		}
	}
	return ""
}

// rewriteLine rewrites the link targets in one line outside a fence,
// skipping inline code spans.
func rewriteLine(line, dir string, isDir func(string) bool) string {
	var b strings.Builder
	for i := 0; i < len(line); {
		switch {
		case line[i] == '`':
			n := 0
			for i+n < len(line) && line[i+n] == '`' {
				n++
			}
			ticks := line[i : i+n]
			end := strings.Index(line[i+n:], ticks)
			if end < 0 { // no closing run: literal backticks
				b.WriteString(ticks)
				i += n
				continue
			}
			stop := i + n + end + n
			b.WriteString(line[i:stop])
			i = stop
		case strings.HasPrefix(line[i:], "]("):
			end := closeParen(line, i+2)
			if end < 0 {
				b.WriteString("](")
				i += 2
				continue
			}
			target := line[i+2 : end]
			b.WriteString("](")
			b.WriteString(rewriteTarget(target, dir, isDir))
			b.WriteByte(')')
			i = end + 1
		default:
			b.WriteByte(line[i])
			i++
		}
	}
	return b.String()
}

// closeParen returns the index of the ) that closes a link target starting
// at i (parentheses inside it nest), or -1.
func closeParen(s string, i int) int {
	depth := 0
	for ; i < len(s); i++ {
		switch s[i] {
		case '(':
			depth++
		case ')':
			if depth == 0 {
				return i
			}
			depth--
		case '\n':
			return -1
		}
	}
	return -1
}

// rewriteTarget returns a link target (a URL, optionally followed by a
// title) with a repository path made a GitHub URL.
func rewriteTarget(target, dir string, isDir func(string) bool) string {
	url, title, _ := strings.Cut(target, " ")
	if title != "" {
		title = " " + title
	}
	if url == "" || strings.HasPrefix(url, "#") || strings.HasPrefix(url, "/") ||
		strings.HasPrefix(url, "<") || hasScheme(url) {
		return target
	}
	p, frag, _ := strings.Cut(url, "#")
	if frag != "" {
		frag = "#" + frag
	}
	repoPath := path.Join(bookDir, dir, p)
	if path.Dir(repoPath) == bookDir && strings.HasSuffix(repoPath, ".md") {
		return target // a chapter: mdBook links it
	}
	if strings.HasPrefix(repoPath, "../") || repoPath == ".." {
		return target // outside the repository: leave it
	}
	kind := "blob"
	if strings.HasSuffix(p, "/") || (isDir != nil && isDir(repoPath)) {
		kind = "tree"
	}
	return Repo + "/" + kind + "/main/" + repoPath + frag + title
}

// hasScheme reports whether url starts with a scheme ("https:", "mailto:").
func hasScheme(url string) bool {
	for i := 0; i < len(url); i++ {
		c := url[i]
		switch {
		case c == ':':
			return i > 0
		case c >= 'a' && c <= 'z', c >= 'A' && c <= 'Z', i > 0 && (c >= '0' && c <= '9' || c == '+' || c == '-' || c == '.'):
		default:
			return false
		}
	}
	return false
}
