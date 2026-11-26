// Package strings is the gogo string helper library, a small subset of Go's
// strings package written in the gogo subset and compiled into every image
// that imports it.
//
// Strings are byte sequences; the subset has no rune decoding, so every
// function here works on bytes. Where Go's behavior depends on Unicode
// (TrimSpace, ToLower, EqualFold, Split with an empty separator) the
// difference is stated on the function.
package strings

// IndexByte returns the index of the first c in s, or -1.
func IndexByte(s string, c byte) int {
	for i := 0; i < len(s); i++ {
		if s[i] == c {
			return i
		}
	}
	return -1
}

// match reports whether sub occurs in s at i (i+len(sub) <= len(s)),
// comparing bytes in place: a substring s[i:i+len(sub)] would be a new
// heap string per comparison.
func match(s string, i int, sub string) bool {
	for j := 0; j < len(sub); j++ {
		if s[i+j] != sub[j] {
			return false
		}
	}
	return true
}

// Index returns the index of the first instance of sub in s, or -1. An
// empty sub is found at 0.
func Index(s, sub string) int {
	n := len(sub)
	for i := 0; i+n <= len(s); i++ {
		if match(s, i, sub) {
			return i
		}
	}
	return -1
}

// Cut slices s around the first instance of sep, returning the text before
// and after it. found reports whether sep appears in s; if not, Cut returns
// s, "", false.
func Cut(s, sep string) (before, after string, found bool) {
	i := Index(s, sep)
	if i < 0 {
		return s, "", false
	}
	return s[:i], s[i+len(sep):], true
}

// HasPrefix reports whether s begins with prefix.
func HasPrefix(s, prefix string) bool {
	return len(s) >= len(prefix) && match(s, 0, prefix)
}

// HasSuffix reports whether s ends with suffix.
func HasSuffix(s, suffix string) bool {
	return len(s) >= len(suffix) && match(s, len(s)-len(suffix), suffix)
}

// isSpace reports whether c is an ASCII white space byte.
func isSpace(c byte) bool {
	return c == ' ' || (c >= '\t' && c <= '\r')
}

// TrimSpace returns s without leading and trailing white space. Only ASCII
// white space (space, \t, \n, \v, \f, \r) is trimmed; Go also trims
// Unicode spaces such as U+0085 and U+00A0.
func TrimSpace(s string) string {
	i, j := 0, len(s)
	for i < j && isSpace(s[i]) {
		i++
	}
	for j > i && isSpace(s[j-1]) {
		j--
	}
	return s[i:j]
}

// lower returns c with an ASCII upper case letter turned to lower case.
func lower(c byte) byte {
	if c >= 'A' && c <= 'Z' {
		return c + ('a' - 'A')
	}
	return c
}

// ToLower returns s with its ASCII upper case letters mapped to lower case.
// Other bytes, including non-ASCII ones, are left alone.
func ToLower(s string) string {
	for i := 0; i < len(s); i++ {
		if lower(s[i]) != s[i] {
			b := []byte(s)
			for ; i < len(b); i++ {
				b[i] = lower(b[i])
			}
			return string(b)
		}
	}
	return s
}

// EqualFold reports whether s and t are equal when ASCII letters are
// compared without regard to case. Go's version folds all of Unicode.
func EqualFold(s, t string) bool {
	if len(s) != len(t) {
		return false
	}
	for i := 0; i < len(s); i++ {
		if lower(s[i]) != lower(t[i]) {
			return false
		}
	}
	return true
}

// Split slices s into all substrings separated by sep and returns them. If
// s does not contain sep and sep is not empty, the result is []string{s}.
// With an empty sep, Split splits s into its bytes (Go splits into UTF-8
// sequences; the subset has no rune decoding), and an empty s then gives an
// empty slice.
func Split(s, sep string) []string {
	var out []string
	if len(sep) == 0 {
		for i := 0; i < len(s); i++ {
			out = append(out, s[i:i+1])
		}
		return out
	}
	for {
		i := Index(s, sep)
		if i < 0 {
			return append(out, s)
		}
		out = append(out, s[:i])
		s = s[i+len(sep):]
	}
}
