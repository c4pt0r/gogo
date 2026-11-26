// There is no error type in the subset. Report failure with a bool, or
// with a named int type of error codes, as gogo/fs does with fs.Err.
package main

import "gogo/strconv"

// ParseErr is a small set of error codes; 0 means success.
type ParseErr int

const (
	OK ParseErr = iota
	Empty
	BadNumber
	OutOfRange
)

// describe names a code for messages.
func describe(e ParseErr) string {
	if e == OK {
		return "ok"
	} else if e == Empty {
		return "empty input"
	} else if e == BadNumber {
		return "not a number"
	}
	return "out of range"
}

// parsePercent parses "0".."100".
func parsePercent(s string) (int, ParseErr) {
	if s == "" {
		return 0, Empty
	}
	n, ok := strconv.Atoi(s)
	if !ok {
		return 0, BadNumber
	}
	if n < 0 || n > 100 {
		return 0, OutOfRange
	}
	return n, OK
}

func main() {
	for _, s := range []string{"42", "", "x1", "250"} {
		n, err := parsePercent(s)
		if err != OK {
			println("["+s+"]", "error:", describe(err), int(err))
			continue
		}
		println("["+s+"]", "=", n)
	}
}
