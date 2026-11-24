// Package gogo carries the embedded library sources used by the compiler.
package gogo

import "embed"

// StubFS holds the Go source of the gogo libraries, one directory per
// package: actor/ (gogo/actor, API stubs the compiler replaces with VM
// instructions) and supervisor/ (gogo/supervisor, compiled into the image).
//
//go:embed actor/*.go supervisor/*.go
var StubFS embed.FS
