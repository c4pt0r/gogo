// Command sitecheck checks the website make site writes (make site-check):
//
//	go run ./tools/sitecheck build/site
//
// In every .html file under the directory it fails on
//   - a relative link to a file that is not there (a directory needs its
//     index.html), or to an #anchor that the target page has no id for;
//   - markdown that was not converted, as a heuristic: a "](", "```" or "**"
//     in the text outside <pre>, <code>, <script> and <style>.
//
// Links with a scheme (https:, mailto:) or a leading / are not checked.
// 404.html is skipped: mdBook gives it a <base href="/">, so its relative
// links resolve against the site root wherever the site is served.
package main

import (
	"fmt"
	"html"
	"io/fs"
	"net/url"
	"os"
	"path"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
)

func main() {
	if len(os.Args) != 2 {
		fmt.Fprintln(os.Stderr, "usage: sitecheck <site dir>")
		os.Exit(2)
	}
	problems, pages, err := Check(os.DirFS(os.Args[1]))
	if err != nil {
		fmt.Fprintln(os.Stderr, "sitecheck:", err)
		os.Exit(2)
	}
	for _, p := range problems {
		fmt.Println("sitecheck:", p)
	}
	if len(problems) > 0 {
		os.Exit(1)
	}
	fmt.Printf("sitecheck: %d pages, links and anchors ok\n", pages)
}

var (
	idRe   = regexp.MustCompile(`\s(?:id|name)\s*=\s*"([^"]*)"`)
	hrefRe = regexp.MustCompile(`\s(?:href|src)\s*=\s*"([^"]*)"`)
	codeRe = regexp.MustCompile(`(?is)<(pre|code|script|style)\b.*?</(pre|code|script|style)>`)
	tagRe  = regexp.MustCompile(`(?s)<[^>]*>`)
)

// Check checks every .html file in site and returns its problems, sorted,
// and the number of pages checked.
func Check(site fs.FS) ([]string, int, error) {
	pages := map[string]string{} // path -> content
	err := fs.WalkDir(site, ".", func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if !d.IsDir() && strings.HasSuffix(p, ".html") {
			b, err := fs.ReadFile(site, p)
			if err != nil {
				return err
			}
			pages[p] = string(b)
		}
		return nil
	})
	if err != nil {
		return nil, 0, err
	}
	ids := map[string]map[string]bool{}
	idsOf := func(p string) map[string]bool {
		if m, ok := ids[p]; ok {
			return m
		}
		m := map[string]bool{}
		for _, s := range idRe.FindAllStringSubmatch(pages[p], -1) {
			m[html.UnescapeString(s[1])] = true
		}
		ids[p] = m
		return m
	}
	var problems []string
	n := 0
	for p, content := range pages {
		if path.Base(p) == "404.html" {
			continue
		}
		n++
		for _, s := range hrefRe.FindAllStringSubmatch(content, -1) {
			if msg := checkLink(site, pages, idsOf, p, html.UnescapeString(s[1])); msg != "" {
				problems = append(problems, p+": "+msg)
			}
		}
		text := tagRe.ReplaceAllString(codeRe.ReplaceAllString(content, ""), "")
		for _, m := range []string{"](", "```", "**"} {
			if strings.Contains(html.UnescapeString(text), m) {
				problems = append(problems, fmt.Sprintf("%s: unconverted markdown %q outside code", p, m))
			}
		}
	}
	sort.Strings(problems)
	return problems, n, nil
}

// checkLink checks one href or src of page p and returns a problem, or "".
func checkLink(site fs.FS, pages map[string]string, idsOf func(string) map[string]bool, p, link string) string {
	if link == "" || strings.HasPrefix(link, "/") || strings.HasPrefix(link, "{{") {
		return ""
	}
	if u, err := url.Parse(link); err != nil {
		return fmt.Sprintf("bad link %q", link)
	} else if u.Scheme != "" || u.Host != "" {
		return ""
	}
	target, frag, _ := strings.Cut(link, "#")
	target, _, _ = strings.Cut(target, "?")
	file := p
	if target != "" {
		t, err := url.PathUnescape(target)
		if err != nil {
			return fmt.Sprintf("bad link %q", link)
		}
		file = path.Join(path.Dir(p), t)
		if strings.HasPrefix(file, "../") || file == ".." {
			return fmt.Sprintf("link %q leaves the site", link)
		}
		st, err := fs.Stat(site, file)
		if err != nil {
			return fmt.Sprintf("link %q: no file %s", link, filepath.FromSlash(file))
		}
		if st.IsDir() {
			file = path.Join(file, "index.html")
			if _, err := fs.Stat(site, file); err != nil {
				return fmt.Sprintf("link %q: directory without index.html", link)
			}
		}
	}
	if frag == "" {
		return ""
	}
	if _, ok := pages[file]; !ok {
		return "" // an anchor into a non-HTML file: not checked
	}
	f, err := url.PathUnescape(frag)
	if err != nil {
		f = frag
	}
	if !idsOf(file)[f] {
		return fmt.Sprintf("link %q: no anchor #%s in %s", link, f, file)
	}
	return ""
}
