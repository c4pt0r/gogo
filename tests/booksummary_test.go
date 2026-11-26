package tests

import (
	"os"
	"path/filepath"
	"regexp"
	"slices"
	"testing"
)

// TestBookSummary keeps docs/book/SUMMARY.md, the table of contents mdBook
// builds the website from (make site), in step with the book: every
// chapter file docs/book/NN-*.md is listed, in the same order as the
// chapters in README.md's Contents, and every file listed exists. A new
// chapter that is not added to SUMMARY.md would be missing from the site.
func TestBookSummary(t *testing.T) {
	dir := filepath.Join("..", "docs", "book")
	chapters, err := filepath.Glob(filepath.Join(dir, "[0-9][0-9]-*.md"))
	if err != nil {
		t.Fatal(err)
	}
	if len(chapters) == 0 {
		t.Fatal("no chapters in docs/book")
	}
	for i, c := range chapters {
		chapters[i] = filepath.Base(c)
	}
	link := regexp.MustCompile(`\]\(([0-9][0-9]-[^)#]*\.md)\)`)
	files := func(name string, re *regexp.Regexp) []string {
		b, err := os.ReadFile(filepath.Join(dir, name))
		if err != nil {
			t.Fatal(err)
		}
		var out []string
		for _, m := range re.FindAllStringSubmatch(string(b), -1) {
			out = append(out, m[1])
		}
		return out
	}
	summary := files("SUMMARY.md", link)
	for _, c := range chapters {
		if !slices.Contains(summary, c) {
			t.Errorf("docs/book/%s is not in docs/book/SUMMARY.md: add it there (and to README.md's Contents)", c)
		}
	}
	for _, s := range summary {
		if !slices.Contains(chapters, s) {
			t.Errorf("docs/book/SUMMARY.md lists %s, which does not exist", s)
		}
	}
	contents := files("README.md", regexp.MustCompile(`(?m)^\d+\. \[[^\]]*\]\(([0-9][0-9]-[^)#]*\.md)\)`))
	if !slices.Equal(summary, contents) {
		t.Errorf("docs/book/SUMMARY.md lists the chapters in another order than README.md's Contents:\nSUMMARY.md %q\nREADME.md  %q", summary, contents)
	}
}
