package main

import "gogo/strings"

func show(parts []string) {
	println(len(parts))
	for _, p := range parts {
		println("  [" + p + "]")
	}
}

func main() {
	// Index, IndexByte
	println(strings.Index("chicken", "ken"), strings.Index("chicken", "dmr"), strings.Index("chicken", ""), strings.Index("", ""), strings.Index("", "a"))
	println(strings.Index("abc", "abcd"), strings.Index("aaab", "aab"), strings.Index("abab", "ab"), strings.Index("xyz", "z"), strings.Index("你好世界", "世"))
	println(strings.IndexByte("golang", 'l'), strings.IndexByte("golang", 'x'), strings.IndexByte("", 'a'), strings.IndexByte("a", 'a'), strings.IndexByte("ab\x00c", 0))

	// Cut
	b, a, f := strings.Cut("key=value=x", "=")
	println(b, a, f)
	b, a, f = strings.Cut("novalue", "=")
	println(b, a, f)
	b, a, f = strings.Cut("=lead", "=")
	println(b, a, f, len(b), len(a))
	b, a, f = strings.Cut("trail=", "=")
	println(b, a, f, len(b), len(a))
	b, a, f = strings.Cut("abc", "")
	println(b, a, f, len(b), len(a))
	b, a, f = strings.Cut("", "x")
	println(b, a, f, len(b), len(a))
	b, a, f = strings.Cut("a::b", "::")
	println(b, a, f)

	// HasPrefix, HasSuffix
	println(strings.HasPrefix("golang", "go"), strings.HasPrefix("golang", "lang"), strings.HasPrefix("go", "golang"), strings.HasPrefix("", ""), strings.HasPrefix("a", ""), strings.HasPrefix("", "a"))
	println(strings.HasSuffix("golang", "lang"), strings.HasSuffix("golang", "go"), strings.HasSuffix("ng", "lang"), strings.HasSuffix("", ""), strings.HasSuffix("a", ""), strings.HasSuffix("", "a"), strings.HasSuffix("golang", "golang"))

	// TrimSpace
	println("[" + strings.TrimSpace("  \t hello world \r\n") + "]")
	println("[" + strings.TrimSpace("") + "]")
	println("[" + strings.TrimSpace(" \t\n\v\f\r ") + "]")
	println("[" + strings.TrimSpace("a b") + "]")
	println("["+strings.TrimSpace("  x")+"]", "["+strings.TrimSpace("x  ")+"]", "["+strings.TrimSpace("x")+"]")
	println(len(strings.TrimSpace("\vq\f")))

	// ToLower, EqualFold
	println(strings.ToLower("Hello, WORLD 123 @[`{"))
	println(strings.ToLower("already lower"), strings.ToLower("") == "", strings.ToLower("Z") == "z")
	println(strings.EqualFold("Go", "GO"), strings.EqualFold("Go", "go!"), strings.EqualFold("", ""), strings.EqualFold("a@", "A`"), strings.EqualFold("Hello", "hELLO"), strings.EqualFold("abc", "abd"))

	// Split
	show(strings.Split("a,b,c", ","))
	show(strings.Split("a,b,c", "!"))
	show(strings.Split("", ","))
	show(strings.Split(",", ","))
	show(strings.Split(",a,,b,", ","))
	show(strings.Split("a--b--c", "--"))
	show(strings.Split("aaa", "aa"))
	show(strings.Split("abc", "abc"))
	show(strings.Split("abc", "abcd"))
	show(strings.Split("abc", ""))
	show(strings.Split("", ""))
	parts := strings.Split("/usr/local/bin", "/")
	println(len(parts), parts[0] == "", parts[3])
}
