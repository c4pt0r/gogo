// Package gogo carries the embedded library sources used by the compiler.
package gogo

import "embed"

// StubFS holds the Go source of the gogo libraries, one directory per
// package: actor/, port/, bytes/, netapi/ and config/ (gogo/actor,
// gogo/port, gogo/bytes, gogo/net and gogo/config, API stubs the compiler
// replaces with VM instructions) and supervisor/
// (gogo/supervisor, compiled into the image), strings/ and strconv/ (gogo/strings
// and gogo/strconv, compiled into the image like supervisor), dhcp/,
// dns/ and http/ (gogo/dhcp, gogo/dns and gogo/http, likewise; Go build
// tag gogo, since they import gogo/net). Libraries may import the stubs and other libraries,
// without cycles.
//
//go:embed actor/*.go port/*.go bytes/*.go netapi/*.go config/*.go dhcp/*.go dns/*.go http/*.go supervisor/*.go strings/*.go strconv/*.go
var StubFS embed.FS
