// Package gogo carries the embedded actor API stubs used by the compiler.
package gogo

import "embed"

// StubFS holds the Go source of the gogo/actor package.
//
//go:embed actor/*.go
var StubFS embed.FS
