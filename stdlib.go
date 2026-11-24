// Package gogo carries the embedded library sources used by the compiler.
package gogo

import "embed"

// StubFS holds the Go source of the gogo libraries, one directory per
// package: actor/, port/ and bytes/ (gogo/actor, gogo/port and gogo/bytes,
// API stubs the compiler replaces with VM instructions) and supervisor/
// (gogo/supervisor, compiled into the image), strings/ and strconv/ (gogo/strings
// and gogo/strconv, compiled into the image like supervisor).
//
//go:embed actor/*.go port/*.go bytes/*.go supervisor/*.go strings/*.go strconv/*.go
var StubFS embed.FS
