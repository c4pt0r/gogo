package main

import (
	"reflect"
	"testing"
	"testing/fstest"
)

func file(s string) *fstest.MapFile { return &fstest.MapFile{Data: []byte(s)} }

func TestCheckClean(t *testing.T) {
	site := fstest.MapFS{
		"index.html": file(`<a href="book/index.html">book</a> <a href="book/">dir</a>
<a href="https://github.com/c4pt0r/gogo">gh</a> <a href="mailto:a@b">m</a> <a href="/abs">abs</a>
<link href="style.css"> <a href="#top" id="top">top</a>`),
		"style.css":           file(""),
		"book/index.html":     file(`<a href="04-actors.html#call-and-reply">c</a> <a href="../index.html">home</a> <a href="04-actors.html?x=1">q</a>`),
		"book/04-actors.html": file(`<h2 id="call-and-reply">Call</h2><a href="#call-and-reply">self</a> <a href="#a%20b">esc</a><i id="a b"></i> <a href="x.css#frag">css</a> <a href="#amp&amp;x">amp</a><b id="amp&amp;x"></b>`),
		"book/x.css":          file(""),
		// mdBook's 404 page has <base href="/">: skipped
		"book/404.html": file(`<a href="nowhere.html">x</a>`),
		// markdown markers inside code are fine
		"book/code.html": file("<pre><code>[a](b) ```</code></pre><p>a <code>**x**](y)</code></p><script>x = '](';</script>"),
	}
	problems, n, err := Check(site)
	if err != nil {
		t.Fatal(err)
	}
	if len(problems) != 0 {
		t.Errorf("problems: %q", problems)
	}
	if n != 4 {
		t.Errorf("pages checked = %d, want 4", n)
	}
}

func TestCheckProblems(t *testing.T) {
	site := fstest.MapFS{
		"index.html":      file(`<a href="book/missing.html">m</a> <a href="book/02-x.html#nope">a</a> <a href="#gone">g</a> <a href="../up.html">u</a> <a href="empty/">d</a>`),
		"empty/x.txt":     file(""),
		"book/02-x.html":  file(`<h2 id="yes">Y</h2>`),
		"book/raw.html":   file("<p>see [x](y.md) and</p>\n<p>```go **b**</p>"),
		"book/sub/a.html": file(`<img src="../pic.png">`),
	}
	problems, _, err := Check(site)
	if err != nil {
		t.Fatal(err)
	}
	want := []string{
		`book/raw.html: unconverted markdown "**" outside code`,
		`book/raw.html: unconverted markdown "](" outside code`,
		"book/raw.html: unconverted markdown \"```\" outside code",
		`book/sub/a.html: link "../pic.png": no file book/pic.png`,
		`index.html: link "#gone": no anchor #gone in index.html`,
		`index.html: link "../up.html" leaves the site`,
		`index.html: link "book/02-x.html#nope": no anchor #nope in book/02-x.html`,
		`index.html: link "book/missing.html": no file book/missing.html`,
		`index.html: link "empty/": directory without index.html`,
	}
	if !reflect.DeepEqual(problems, want) {
		t.Errorf("problems:\n%q\nwant\n%q", problems, want)
	}
}
